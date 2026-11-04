using System.Text;
using IslandBridge.Core.Text;

namespace IslandBridge.Tests.Text;

public sealed class TextSanitizerTests
{
    [Theory]
    [InlineData("plain text", "plain text")]
    [InlineData("  leading and trailing  ", "leading and trailing")]
    [InlineData("line one\r\nline two\nthree", "line one line two three")]
    [InlineData("tabs\t\tand   spaces", "tabs and spaces")]
    [InlineData("bell\u0007 and null\u0000", "bell and null")]
    [InlineData("\u202Ereversed\u202C", "reversed")]
    [InlineData("\u2066isolate\u2069 \u200Emark\u200F", "isolate mark")]
    [InlineData("line\u2028separator", "line separator")]
    [InlineData("emoji 👍🏽 ok", "emoji 👍🏽 ok")]
    [InlineData("family 👨\u200D👩\u200D👧", "family 👨\u200D👩\u200D👧")]
    [InlineData("Ahoj, ako sa máš?", "Ahoj, ako sa máš?")]
    public void Cleans_text(string input, string expected)
    {
        Assert.Equal(expected, TextSanitizer.Sanitize(input, 200));
    }

    [Fact]
    public void Truncates_without_splitting_surrogate_pairs()
    {
        var result = TextSanitizer.Sanitize("ab😀", 3);

        Assert.Equal("ab", result);
    }

    [Fact]
    public void Truncation_never_leaves_a_trailing_space()
    {
        Assert.Equal("abc", TextSanitizer.Sanitize("abc def", 4));
    }

    [Fact]
    public void Collapses_runs_of_replacement_characters()
    {
        Assert.Equal("a\uFFFDb", TextSanitizer.Sanitize([0x61, 0xFF, 0xFE, 0xFD, 0x62], 10));
    }

    [Fact]
    public void Decodes_utf8()
    {
        Assert.Equal("Čau", TextSanitizer.Sanitize(Encoding.UTF8.GetBytes("Čau"), 10));
    }

    [Fact]
    public void Zero_length_gives_empty()
    {
        Assert.Equal(string.Empty, TextSanitizer.Sanitize("anything", 0));
        Assert.Equal(string.Empty, TextSanitizer.Sanitize([], 10));
    }
}
