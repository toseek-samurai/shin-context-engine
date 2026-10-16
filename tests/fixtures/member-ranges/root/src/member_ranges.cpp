class CppRange {
public:
    std::string multilineMember(
        const std::string& value,
        int retries) {
        return value + std::to_string(retries);
    }
};

int oneLine() { return 1; }
int sameLine() { return 1; } int sameLine(int value) { return value; }
void readMetadata(const struct stat& info) { inspect(info); }
class Constructed {
public:
    Constructed(int value) : policy_(value) {}
    int policy_;
};
