import com.sun.source.util.JavacTask;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import javax.tools.Diagnostic;
import javax.tools.DiagnosticCollector;
import javax.tools.JavaFileObject;
import javax.tools.ToolProvider;

// Parse only: no attribution, annotation processing, code generation or execution.
class JavaSyntaxCheck {
    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        List<Path> files;
        try (var paths = Files.walk(root, 32)) {
            var entries = paths.limit(2049).toList();
            if (entries.size() > 2048 || entries.stream().anyMatch(Files::isSymbolicLink))
                throw new IllegalArgumentException("syntax-source-budget-or-link");
            files = entries.stream().filter(Files::isRegularFile)
                    .filter(path -> path.toString().endsWith(".java")).toList();
        }
        long bytes = 0;
        for (Path file : files) {
            long size = Files.size(file);
            bytes += size;
            if (size > 1048576 || bytes > 8388608) throw new IllegalArgumentException("syntax-byte-budget");
        }
        if (files.isEmpty() || files.size() > 512) throw new IllegalArgumentException("syntax-file-budget");
        var compiler = ToolProvider.getSystemJavaCompiler();
        var diagnostics = new DiagnosticCollector<JavaFileObject>();
        try (var manager = compiler.getStandardFileManager(diagnostics, null, java.nio.charset.StandardCharsets.UTF_8)) {
            var task = (JavacTask) compiler.getTask(null, manager, diagnostics,
                    List.of("-proc:none"), null, manager.getJavaFileObjectsFromPaths(files));
            for (var ignored : task.parse()) { }
        }
        var errors = diagnostics.getDiagnostics().stream()
                .filter(d -> d.getKind() == Diagnostic.Kind.ERROR).toList();
        errors.stream().limit(20).forEach(System.err::println);
        System.out.println("{\"ok\":" + errors.isEmpty() + ",\"files\":" + files.size()
                + ",\"bytes\":" + bytes + ",\"errors\":" + errors.size() + "}");
        if (!errors.isEmpty()) System.exit(1);
    }
}
