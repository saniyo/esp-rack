#ifndef WEB_FIELD_VOCAB_H
#define WEB_FIELD_VOCAB_H

// ─────────────────────────────────────────────────────────────────────────
//  SINGLE SOURCE OF TRUTH for the web field vocabulary.
//
//  Every field "type" token and every option-string key the web layer emits
//  lives HERE as a named constant — nowhere else in the backend should a
//  field type or `o`-string key appear as a raw string literal. The fluent
//  Form builders (web/Form.h) reference these; module code never types
//  "number" or "rowFillFallback" by hand again.
//
//  This MIRRORS the frontend parser, which is the other half of the contract:
//    ui/src/components/dynamic-component/utils/fieldParser.ts
//      • fieldConfigurations  — which option keys each type accepts
//      • optionParsers        — how each key's value is decoded
//  Keep the two in lockstep. The wire format is a `;`-separated option string
//  "<type>;<af>;key=value;flag;…" that the frontend parses ORDER-INDEPENDENTLY
//  (it scans every segment into a key→value map), so emission order is free.
//
//  TODO(codegen): generate fieldConfigurations.ts FROM this header (or both
//  from a shared spec) so C++ and TS can never drift. Until then this header
//  is hand-kept in sync and is the authoritative backend copy.
//
//  ── Wire-key length review (the keys are FIXED by the frontend parser; to
//     shorten any, both sides change together — do it via the codegen step) ──
//     1–2 ch : r rw mn mx f st pl to l
//     short  : url ref base mode cols icon unit level badge color lines
//     medium : accept layout legend avatar method dtmode primary tooltip
//              confirm rowFill rowNav initial maxRows showIf options
//     long   : colorMap maxPoints secondary loadingIf hideAvatar cardTitle
//              uploadBase withFields refetchForm
//     longest: rowFillFallback (15) cardDeletable (13)
// ─────────────────────────────────────────────────────────────────────────

namespace web {

// ── Field type tokens — first segment of the option string. ──
// One entry per renderer element. Matches fieldConfigurations keys.
namespace FieldType {
constexpr const char* Text      = "text";
constexpr const char* Secret    = "secret";
constexpr const char* Number    = "number";
constexpr const char* Slider    = "slider";
constexpr const char* Checkbox  = "checkbox";
constexpr const char* Switch    = "switch";
constexpr const char* Button    = "button";
constexpr const char* Dropdown  = "dropdown";
constexpr const char* Textarea  = "textarea";
constexpr const char* Radio     = "radio";
constexpr const char* Trend     = "trend";
constexpr const char* Files     = "files";
constexpr const char* Upload    = "upload";
constexpr const char* Table     = "table";
constexpr const char* DateTime  = "datetime";
constexpr const char* Action    = "action";
constexpr const char* Message   = "message";
constexpr const char* TimeZones = "timezones";
constexpr const char* Ip        = "ip";
}  // namespace FieldType

// ── Access flag — second segment. r = read-only, rw = editable. ──
namespace Access {
constexpr const char* Read      = "r";
constexpr const char* ReadWrite = "rw";
}  // namespace Access

// ── Option keys — the `key=value` (or bare-flag) segments. ──
// Grouped by purpose; the comment after each notes which field types accept
// it on the frontend (so the per-type builders expose exactly the right ones).
namespace Opt {
// Universal — accepted by EVERY field type (live on the FieldSpec base).
constexpr const char* Label         = "l";            // label override (kept 1-char on purpose)
constexpr const char* Icon          = "icon";         // all
constexpr const char* Color         = "color";        // all
constexpr const char* ShowIf        = "showIf";       // all  — "field:value"
constexpr const char* Card          = "card";         // all  — group id
constexpr const char* CardTitle     = "cardTitle";    // all
constexpr const char* CardDeletable = "cardDeletable";// all  — "=1"
constexpr const char* HideAvatar    = "hideAvatar";   // all  — "=1"
constexpr const char* RefetchForm   = "refetchForm";  // all  — "=1"
constexpr const char* LoadingIf     = "loadingIf";    // all  — "field:prefix"

// Value-ish, on input-like types.
constexpr const char* Placeholder   = "pl";           // text/secret/number/slider/checkbox/button/switch/dropdown/textarea/radio/ip
constexpr const char* Unit          = "unit";         // text/number/ip
constexpr const char* ColorMap      = "colorMap";     // text/secret/number/slider/checkbox/button/switch/dropdown/textarea/radio/ip

// Number / slider.
constexpr const char* Min           = "mn";           // number/slider
constexpr const char* Max           = "mx";           // number/slider
constexpr const char* Format        = "f";            // number
constexpr const char* Step          = "st";           // slider

// Dropdown / radio.
constexpr const char* Options       = "options";      // dropdown/radio — "Label:val,Label:val"

// Trend.
constexpr const char* TrendXAxis    = "xAxis";        // trend
constexpr const char* Lines         = "lines";        // trend
constexpr const char* Legend        = "legend";       // trend — bare flag
constexpr const char* Tooltip       = "tooltip";      // trend — bare flag
constexpr const char* MaxPoints     = "maxPoints";    // trend
constexpr const char* Mode          = "mode";         // trend / table

// Files / upload.
constexpr const char* Base          = "base";         // files
constexpr const char* Initial       = "initial";      // files
constexpr const char* UploadBase    = "uploadBase";   // files
constexpr const char* Url           = "url";          // upload / action
constexpr const char* Accept        = "accept";       // upload

// Table.
constexpr const char* Cols            = "cols";            // table
constexpr const char* MaxRows         = "maxRows";         // table
constexpr const char* RowFill         = "rowFill";         // table
constexpr const char* RowFillFallback = "rowFillFallback"; // table
constexpr const char* RowNav          = "rowNav";          // table
constexpr const char* Layout          = "layout";          // table — "grid|cards"
constexpr const char* Primary         = "primary";         // table cards
constexpr const char* Secondary       = "secondary";       // table cards
constexpr const char* Avatar          = "avatar";          // table cards
constexpr const char* Badge           = "badge";           // table cards — "field|unit|icon"

// Datetime.
constexpr const char* DateTimeMode  = "dtmode";       // datetime — "date|time|datetime"

// Action.
constexpr const char* Method        = "method";       // action
constexpr const char* Confirm       = "confirm";      // action
constexpr const char* ActionRef     = "ref";          // action — manifest action id
constexpr const char* WithFields    = "withFields";   // action — "param=field,..."

// Message.
constexpr const char* Level         = "level";        // message — Alert severity
}  // namespace Opt

}  // namespace web

#endif  // WEB_FIELD_VOCAB_H
