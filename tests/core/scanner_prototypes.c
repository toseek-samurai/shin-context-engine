#include <stddef.h>
#include <stdio.h>

void *tree_sitter_typescript_external_scanner_create(void);
void *tree_sitter_tsx_external_scanner_create(void);
void *tree_sitter_cpp_external_scanner_create(void);
void tree_sitter_cpp_external_scanner_destroy(void *payload);

int main(void) {
    void *(*volatile create)(void) = tree_sitter_typescript_external_scanner_create;
    if (create() != NULL) return 1;
    create = tree_sitter_cpp_external_scanner_create;
    void *scanner = create();
    if (scanner == NULL) return 1;
    tree_sitter_cpp_external_scanner_destroy(scanner);
    create = tree_sitter_tsx_external_scanner_create;
    if (create() != NULL) return 1;
    puts("C11 scanner factory callbacks passed");
    return 0;
}
