using System.Net;
using System.Net.Http.Headers;
using System.Security.Authentication;
using System.Security.Cryptography;
using IslandBridge.Core.Updates;

namespace IslandBridge.Updater;

internal sealed class UpdateException(UpdateFailure failure, string detail) : Exception(detail)
{
    public UpdateFailure Failure { get; } = failure;
}

// HTTPS to the allow-listed GitHub hosts only. Redirects are followed by hand so every hop is
// checked; no cookies, no credentials, no decompression, and every body has a hard size cap.
internal sealed class ReleaseClient : IDisposable
{
    private const int BufferBytes = 64 * 1024;

    private readonly HttpClient _http;

    public ReleaseClient()
    {
        var handler = new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            UseCookies = false,
            AutomaticDecompression = DecompressionMethods.None,
            PreAuthenticate = false,
            Credentials = null,
            ConnectTimeout = TimeSpan.FromSeconds(15),
            PooledConnectionLifetime = TimeSpan.FromMinutes(2),
            MaxResponseHeadersLength = 64,
            MaxConnectionsPerServer = 2,
            SslOptions = { EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13 },
        };

        _http = new HttpClient(handler) { Timeout = Timeout.InfiniteTimeSpan };
        _http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("IslandBridge-Updater", UpdateVersion.Current.ToString()));
        _http.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/octet-stream"));
    }

    public async Task<byte[]> GetSmallAsync(Uri uri, int maxBytes, CancellationToken cancellationToken)
    {
        using var response = await SendAsync(uri, cancellationToken).ConfigureAwait(false);

        if (response.Content.Headers.ContentLength is { } length && length > maxBytes)
        {
            throw new UpdateException(UpdateFailure.Manifest, "small file too large");
        }

        using var stream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        var buffer = new byte[maxBytes + 1];
        var total = 0;

        while (total < buffer.Length)
        {
            var read = await stream.ReadAsync(buffer.AsMemory(total), cancellationToken).ConfigureAwait(false);

            if (read == 0)
            {
                break;
            }

            total += read;
        }

        if (total > maxBytes)
        {
            throw new UpdateException(UpdateFailure.Manifest, "small file too large");
        }

        return buffer[..total];
    }

    public async Task DownloadAsync(Uri uri, Stream destination, long expectedBytes, IncrementalHash hash, CancellationToken cancellationToken)
    {
        using var response = await SendAsync(uri, cancellationToken).ConfigureAwait(false);

        if (response.Content.Headers.ContentLength is { } length && length != expectedBytes)
        {
            throw new UpdateException(UpdateFailure.Download, "unexpected content length");
        }

        using var stream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        var buffer = new byte[BufferBytes];
        long total = 0;

        while (true)
        {
            var read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);

            if (read == 0)
            {
                break;
            }

            total += read;

            if (total > expectedBytes)
            {
                throw new UpdateException(UpdateFailure.Download, "download larger than the manifest says");
            }

            hash.AppendData(buffer, 0, read);
            await destination.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
        }

        if (total != expectedBytes)
        {
            throw new UpdateException(UpdateFailure.Download, "download shorter than the manifest says");
        }
    }

    public void Dispose() => _http.Dispose();

    private async Task<HttpResponseMessage> SendAsync(Uri uri, CancellationToken cancellationToken)
    {
        var current = UpdateEndpoints.IsAllowed(uri) ? uri : throw new UpdateException(UpdateFailure.Network, "address not allowed");

        for (var hop = 0; hop <= UpdateEndpoints.MaxRedirects; hop++)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, current);
            HttpResponseMessage response;

            try
            {
                response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
            }
            catch (HttpRequestException exception)
            {
                throw new UpdateException(UpdateFailure.Network, $"request failed: {exception.HttpRequestError}");
            }

            if (response.StatusCode is HttpStatusCode.MovedPermanently or HttpStatusCode.Found or HttpStatusCode.SeeOther
                or HttpStatusCode.TemporaryRedirect or HttpStatusCode.PermanentRedirect)
            {
                var next = UpdateEndpoints.ResolveRedirect(current, response.Headers.Location);
                response.Dispose();
                current = next ?? throw new UpdateException(UpdateFailure.Network, "redirect to an address that is not allowed");
                continue;
            }

            if (response.StatusCode != HttpStatusCode.OK)
            {
                var status = (int)response.StatusCode;
                response.Dispose();
                throw new UpdateException(UpdateFailure.Network, $"http {status}");
            }

            return response;
        }

        throw new UpdateException(UpdateFailure.Network, "too many redirects");
    }
}
