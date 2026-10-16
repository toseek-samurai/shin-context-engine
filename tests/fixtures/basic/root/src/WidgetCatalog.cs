namespace Fixture.Platform;

public interface IWidgetCatalog
{
    WidgetSnapshot LoadWidget(string id);
}

public sealed record WidgetSnapshot(string Id, string Status);

public sealed class WidgetCatalog : IWidgetCatalog
{
    public static WidgetCatalog operator +(WidgetCatalog left, WidgetCatalog right) => left;

    public string Mode { get; } = "strict";

    public WidgetSnapshot LoadWidget(string id)
    {
        return new WidgetSnapshot(id, "ready");
    }
}
