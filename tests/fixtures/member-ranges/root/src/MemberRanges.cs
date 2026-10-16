public sealed class CsharpRange {
    public string MultilineMember(
        string value,
        int retries)
    {
        return $"{value}:{retries}";
    }

    public CsharpRange(string seed)
    {
        MultilineMember(seed, 1);
    }
}
