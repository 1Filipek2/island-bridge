using System.Runtime.CompilerServices;
using IslandBridge.AncsSpike;

var options = SpikeOptions.Parse(args);
using var cancellation = options.Duration is { } duration ? new CancellationTokenSource(duration) : new CancellationTokenSource();

Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    cancellation.Cancel();
};

SpikeLog.Write($"AncsSpike, native AOT: {!RuntimeFeature.IsDynamicCodeSupported}, show text: {options.ShowText}");

return await new AncsSession(options).RunAsync(cancellation.Token);
