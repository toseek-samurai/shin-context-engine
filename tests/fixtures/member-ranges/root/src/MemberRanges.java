package fixture.ranges;

import fixture.ranges.TopLevelHelper;
import fixture.ranges.MemberRanges.Nested;
import fixture.ranges.*;
import static fixture.ranges.TopLevelHelper.helper;

@Deprecated
final class MemberRanges {
    @Deprecated
    MemberRanges(String seed) {
        use(seed);
    }

    @Deprecated
    private String javaMultiline(
            String value,
            int retryCount) {
        return value + retryCount;
    }

    private String overloaded(String value) {
        return value;
    }

    private String overloaded(int retryCount) {
        return Integer.toString(retryCount);
    }

    void callOnly() {
        run(new Object());
    }

    static class Nested {
    }
}
