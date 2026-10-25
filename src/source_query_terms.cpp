#include "source_query_terms.h"
#include "context_internal.h"

namespace shin::context {
namespace {
bool stop_word(const std::string& term) {
    if (is_query_stop_word(term)) return true;
    static constexpr auto words = std::to_array<std::string_view>({
        "happen", "happens", "happening", "в", "во", "где", "да", "для", "до", "его", "ее", "её",
        "если", "же", "за", "зачем", "и", "из", "или", "их", "к", "как", "какая", "какие", "какой",
        "когда", "который", "ли", "между", "может", "на", "над", "наш", "не", "но", "о", "об",
        "от", "перед", "по", "под", "после", "почему", "при", "про", "с", "со", "так", "то",
        "у", "уже", "что", "чтобы", "это", "этот"
    });
    return std::find(words.begin(), words.end(), term) != words.end();
}

struct Concept { std::string_view stem, english; };
// General software vocabulary only: no repository names, paths, symbols or
// evaluation questions. A finite hint lexicon is not a sentence translator.
// Match a whole word or a reviewed inflection, never an arbitrary prefix.
constexpr auto concepts = std::to_array<Concept>({
    {"абсолютн", "absolute"}, {"актуальн", "current freshness"}, {"архив", "archive"},
    {"асинхронн", "asynchronous"}, {"атрибут", "attribute"}, {"баз", "database"},
    {"байт", "byte"}, {"блокиров", "lock block"}, {"буфер", "buffer"},
    {"верси", "version"}, {"ветк", "branch"}, {"восстанов", "restore resume recover"},
    {"вход", "input"}, {"выход", "output"}, {"вызов", "call invoke"},
    {"готов", "ready"}, {"границ", "boundary"}, {"данн", "data"},
    {"добав", "add append"}, {"документ", "document"}, {"доступ", "access"},
    {"зависим", "dependency"}, {"загруз", "load"}, {"задач", "task"},
    {"запис", "record write"}, {"запрос", "query request"}, {"измен", "change"},
    {"идентификатор", "identifier identity"}, {"индекс", "index"}, {"исключ", "exception exclude"},
    {"исходник", "source"}, {"кандидат", "candidate"}, {"клиент", "client"},
    {"ключ", "key"}, {"код", "code"}, {"количеств", "count"}, {"команд", "command"},
    {"конфигурац", "configuration config"}, {"кэш", "cache"}, {"кеш", "cache"},
    {"лимит", "limit budget"}, {"лог", "log"}, {"маршрут", "route"},
    {"метод", "method"}, {"модел", "model"}, {"нормализ", "normalize"},
    {"обнов", "update refresh"}, {"объект", "object"}, {"огранич", "limit bound"},
    {"отдельн", "independent separate"}, {"отмен", "cancel"}, {"очеред", "queue"},
    {"ошиб", "error failure"}, {"памят", "memory"}, {"параметр", "parameter"},
    {"парол", "password"}, {"повтор", "repeat reuse retry"}, {"поиск", "search query retrieval"},
    {"поисков", "search query retrieval"}, {"полн", "full complete"}, {"пользовател", "user"},
    {"поток", "thread stream"}, {"прав", "permission"}, {"провер", "check verify validate"},
    {"процесс", "process"}, {"пут", "path"}, {"раздел", "separate"},
    {"размер", "size"}, {"разреш", "allow permit"}, {"результат", "result"},
    {"секрет", "secret"}, {"сесси", "session"}, {"сет", "network"},
    {"служб", "service"}, {"сним", "snapshot"}, {"событ", "event"},
    {"соедин", "connect"}, {"сохран", "save retain persist"}, {"состояни", "state"},
    {"строк", "line row string"}, {"структур", "structure"}, {"схем", "schema"},
    {"счетчик", "count counter"}, {"счётчик", "count counter"}, {"таблиц", "table"},
    {"таймаут", "timeout"}, {"текст", "text"}, {"тест", "test"}, {"токен", "token"},
    {"удал", "remove delete"}, {"управ", "control manage"}, {"услов", "condition"},
    {"файл", "file"}, {"фильтр", "filter"}, {"фрагмент", "fragment chunk span"},
    {"функци", "function"}, {"целостност", "integrity"}, {"чита", "read"},
    {"читател", "reader"}, {"чтени", "read"}, {"шифр", "encrypt"},
    {"экспорт", "export"}, {"язык", "language"}
});

bool inflection(std::string_view suffix) {
    static constexpr auto endings = std::to_array<std::string_view>({
        "", "а", "я", "е", "ё", "и", "й", "о", "у", "ю", "ы", "ь", "ей", "ой", "ий",
        "ый", "ая", "яя", "ое", "ые", "ие", "ого", "его", "ому", "ему", "ом", "ем", "им",
        "ым", "ую", "юю", "ых", "их", "ыми", "ими", "ов", "ев", "ам", "ям", "ах", "ях",
        "ами", "ями", "ость", "ости", "остью", "ность", "ности", "нию", "ние", "ния", "ний",
        "нием", "нный", "нная", "нное", "нные", "нного", "нных", "нному", "нным", "нными",
        "ть", "т", "ет", "ют", "ит", "ят", "ется", "ются", "ится", "ятся", "ка", "ки", "ку",
        "кой", "ок", "ков", "кам", "ками", "ках", "ение", "ения", "ений", "ении", "ению",
        "ением", "енный", "енного", "енной", "енные", "енных", "енному", "енным", "енными",
        "ённый", "ённого", "ённой", "ённые", "ённых", "ённому", "ённым", "ёнными",
        "ляет", "ляют", "ляется", "ляются", "ление", "ления", "лении", "лениях", "лять",
        "яет", "яют", "яется", "яются", "ять", "ение", "ений", "енный",
        "ованный", "ованные", "ованных", "ованного", "ует", "уются", "овать", "ования",
        "ивает", "иваются", "ивание", "ивания"
    });
    return std::find(endings.begin(), endings.end(), suffix) != endings.end();
}
}

std::vector<std::string> source_query_terms(const std::vector<std::string>& tokens) {
    std::vector<std::string> result;
    for (const auto& token : tokens) {
        // A single token can name a code symbol even when it is an English stop word.
        if (tokens.size() > 1 && stop_word(token)) continue;
        append_unique_string(result, token);
        for (const auto& entry : concepts) {
            if (!token.starts_with(entry.stem) || !inflection(std::string_view(token).substr(entry.stem.size()))) continue;
            std::istringstream words{std::string(entry.english)};
            for (std::string word; words >> word;) append_unique_string(result, word);
            break;
        }
    }
    return result;
}

bool source_query_distinct_paths_first(const std::vector<std::string>& tokens) {
    // Count the original vocabulary, before translation hints expand one word.
    return std::count_if(tokens.begin(), tokens.end(), [](const auto& token) { return !stop_word(token); }) >= 4;
}
}
