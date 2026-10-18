using System.Collections.Generic;
using System.Text.RegularExpressions;
using Xunit;

namespace Bromelia.Adapters.Tests;

public class RandomIdGeneratorTests
{
    [Fact]
    public void IdsAreLowerCaseVersion4UuidsAndNeverRepeat()
    {
        var ids = new RandomIdGenerator();
        var seen = new HashSet<string>();
        for (int i = 0; i < 10000; i++)
        {
            var id = ids.Next().Value;
            Assert.Matches(new Regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"), id);
            Assert.True(seen.Add(id), id);
        }
    }
}
