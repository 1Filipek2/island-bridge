using System.Globalization;

namespace IslandBridge.Core.Updates;

public enum UpdaterOutcome
{
    Current,
    Available,
    Ready,
    Failed,
}

public enum UpdateFailure
{
    None,
    Usage,
    Busy,
    Elevated,
    NotInstalled,
    NoTrustedKey,
    Network,
    Signature,
    Manifest,
    Download,
    Hash,
    Launch,
}

public enum UpdaterVerb
{
    Invalid,
    Check,
    Install,
}

public sealed record UpdaterCommand(UpdaterVerb Verb, int ParentProcessId)
{
    public const string CheckVerb = "check";
    public const string InstallVerb = "install";
    public const string WaitSwitch = "--wait-pid";

    public static UpdaterCommand Parse(IReadOnlyList<string> args)
    {
        ArgumentNullException.ThrowIfNull(args);

        if (args is [CheckVerb])
        {
            return new UpdaterCommand(UpdaterVerb.Check, 0);
        }

        if (args is [InstallVerb, WaitSwitch, var pid]
            && pid.Length is > 0 and <= 10
            && pid.All(char.IsAsciiDigit)
            && int.TryParse(pid, NumberStyles.None, CultureInfo.InvariantCulture, out var id)
            && id > 0)
        {
            return new UpdaterCommand(UpdaterVerb.Install, id);
        }

        return new UpdaterCommand(UpdaterVerb.Invalid, 0);
    }
}

// One line on the updater's stdout, read by IslandBridge: "current 1.0.0", "available 1.1.0",
// "ready 1.1.0" or "failed network".
public sealed record UpdaterReport(UpdaterOutcome Outcome, UpdateVersion Version, UpdateFailure Failure)
{
    public const int MaxLineLength = 64;

    public static UpdaterReport Current(UpdateVersion version) => new(UpdaterOutcome.Current, version, UpdateFailure.None);

    public static UpdaterReport Available(UpdateVersion version) => new(UpdaterOutcome.Available, version, UpdateFailure.None);

    public static UpdaterReport Ready(UpdateVersion version) => new(UpdaterOutcome.Ready, version, UpdateFailure.None);

    public static UpdaterReport Failed(UpdateFailure failure) => new(UpdaterOutcome.Failed, default, failure);

    public static bool TryParse(string? line, out UpdaterReport? report)
    {
        report = null;

        if (line is null || line.Length > MaxLineLength)
        {
            return false;
        }

        var space = line.IndexOf(' ', StringComparison.Ordinal);

        if (space <= 0)
        {
            return false;
        }

        var word = line[..space];
        var argument = line[(space + 1)..];

        if (word == "failed")
        {
            foreach (var failure in Enum.GetValues<UpdateFailure>())
            {
                if (failure != UpdateFailure.None && string.Equals(Token(failure), argument, StringComparison.Ordinal))
                {
                    report = Failed(failure);
                    return true;
                }
            }

            return false;
        }

        if (!UpdateVersion.TryParse(argument, out var version))
        {
            return false;
        }

        report = word switch
        {
            "current" => Current(version),
            "available" => Available(version),
            "ready" => Ready(version),
            _ => null,
        };

        return report is not null;
    }

    public override string ToString() => Outcome switch
    {
        UpdaterOutcome.Current => $"current {Version}",
        UpdaterOutcome.Available => $"available {Version}",
        UpdaterOutcome.Ready => $"ready {Version}",
        _ => $"failed {Token(Failure)}",
    };

    private static string Token(UpdateFailure failure) => failure switch
    {
        UpdateFailure.Usage => "usage",
        UpdateFailure.Busy => "busy",
        UpdateFailure.Elevated => "elevated",
        UpdateFailure.NotInstalled => "not-installed",
        UpdateFailure.NoTrustedKey => "no-trusted-key",
        UpdateFailure.Network => "network",
        UpdateFailure.Signature => "signature",
        UpdateFailure.Manifest => "manifest",
        UpdateFailure.Download => "download",
        UpdateFailure.Hash => "hash",
        UpdateFailure.Launch => "launch",
        _ => "none",
    };
}
