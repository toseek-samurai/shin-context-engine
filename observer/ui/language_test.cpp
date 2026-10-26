#include "language.h"
#include <stdexcept>
#include <string>

int main(int argc, char** argv) try {
    using namespace shin::ui;
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("UI language contract failed"); };
    if (argc == 3) {
        require(std::string_view(argv[2]) == "en" || std::string_view(argv[2]) == "ru");
        const auto before = language();
        require(terminal_language(argv[1]) == (std::string_view(argv[2]) == "ru" ? Language::ru : Language::en));
        require(language() == before);
        return 0;
    }
    require(argc == 1);
    require(language_for("ru-RU") == Language::ru);
    require(language_for("RU_ru.UTF-8") == Language::ru);
    require(language_for("rubbish") == Language::en);
    require(supported_language("en_US") && supported_language("ru") && !supported_language("de-DE") && !supported_language("english"));
    require(language_for("de-DE") == Language::en);
    require(resolve_language("en", "ru-RU") == Language::en);
    require(resolve_language("ru", "en-US") == Language::ru);
    require(resolve_language("auto", "ru_RU.UTF-8") == Language::ru);
    require(resolve_language("auto", "C.UTF-8") == Language::en);
    for (auto selected : {Language::en, Language::ru}) {
        set_language(selected);
        for (const auto& row : messages) {
            require(*row.en && *row.ru);
            require(std::string_view(tr(row.en)) == (selected == Language::en ? row.en : row.ru));
        }
        require(std::string_view(tr("unrecognized diagnostic code")) == "unrecognized diagnostic code");
    }
    require(std::string_view(tr("Projects")) == "Проекты");
    require(decimal("12.5") == "12,5");
    set_language(Language::en);
    require(std::string_view(tr("Projects")) == "Projects");
    require(decimal("12.5") == "12.5");
    require(std::string_view(tr("Service offline")) == "Service offline");
} catch (const std::exception&) { return 1; }
