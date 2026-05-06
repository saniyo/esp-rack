#ifndef FORMBUILDER_H
#define FORMBUILDER_H

#include <ArduinoJson.h>
#include <vector>
#include <math.h>  // для sin, cos

// ------------------- Глобальні константи для зручності -------------------
static constexpr bool hidden = true;
static constexpr bool visible = false;

// ====================== Теги для NUMBER ======================
struct MinVal { double v; };
inline MinVal minVal(double v) { return MinVal{v}; }

struct MaxVal { double v; };
inline MaxVal maxVal(double v) { return MaxVal{v}; }

struct Format { const char* f; };
inline Format format(const char* f) { return Format{f}; }

// ====================== Теги для SLIDER / COMMON PLACEHOLDER ======================
struct Step { double v; };  // st
inline Step step(double v) { return Step{v}; }

struct Placeholder { const char* t; };  // pl
inline Placeholder placeholder(const char* t) { return Placeholder{t}; }

// ====================== Теги для OPTIONS (dropdown/radio) ======================
struct Opt {
  const char* label;
  int value;
};
inline Opt opt(const char* lbl, int val) { return Opt{lbl, val}; }

// ====================== TREND: line(), xAxis(), lines(), legend(), tooltip(), trendMaxPoints(), mode()  =====
struct TrendLine {
  const char* keyName;
  bool hidden;
  const char* color;
  const char* type;
};

inline TrendLine line(const char* keyName, bool h, const char* color, const char* type) {
  return {keyName, h, color, type};
}
inline TrendLine line(const char* keyName, const char* color, const char* type) {
  return {keyName, false, color, type};
}

struct TrendXAxis { const char* xAxisKey; };
inline TrendXAxis xAxis(const char* v) { return TrendXAxis{v}; }

struct TrendLines { const char* config; };
inline TrendLines lines(const char* c) { return TrendLines{c}; }

struct TrendLegend { bool show; };
inline TrendLegend legend(bool on) { return TrendLegend{on}; }

struct TrendTooltip { bool show; };
inline TrendTooltip tooltip(bool on) { return TrendTooltip{on}; }

struct TrendMaxPoints { int value; };
inline TrendMaxPoints trendMaxPoints(int v) { return TrendMaxPoints{v}; }

struct TrendMode { const char* mode; };  // "lineChart" / "barChart" / "pieChart"
inline TrendMode mode(const char* m) { return TrendMode{m}; }

// Table merge semantics — how the frontend reconciles incoming WS payloads
// against the existing row set:
//   "replace"  (default) — full replace each push
//   "append"  — push rows appended in order, capped at maxRows (oldest drop)
//   "upsert"  — merge by key column (first col); new rows append, existing update
struct TableMode { const char* mode; };
inline TableMode tableMode(const char* m) { return TableMode{m}; }

// Max rows retained by an append/upsert-mode TableField (ignored in replace
// mode because backend already sends the full row set every push).
struct TableMaxRows { int v; };
inline TableMaxRows maxRows(int n) { return TableMaxRows{n}; }

// ====================== Universal extras — шлях для наскрізних опцій ======================
// Будь-який add*Field приймає у variadic-хвіст теги типу icon("Thermostat").
// Тег пропускається полем-специфічним парсером (catch-all шаблон нижче) і
// обробляється applyExtras(field, args...) в кінці add*Field.
struct IconName { const char* name; };
inline IconName icon(const char* n) { return IconName{n}; }

// Explicit label override. By default the React-side parser turns the
// snake_case JSON key into a Title Cased label ("cpu_frequency" →
// "Cpu Frequency"), which mangles acronyms (CPU/MAC/IP/SSID/JWT) and
// concatenated concepts ("heap_free_max_alloc" → "Heap Free Max
// Alloc"). Pass `label("CPU Frequency")` etc. to override the rendered
// label without changing the JSON key on the wire. Universal — every
// add*Field accepts it.
//
// Wire-format key is `l` (single char) to stay friendly to the option
// string size budget. Don't put `;` inside the label text — option
// strings split on `;`.
struct LabelTag { const char* text; };
inline LabelTag label(const char* text) { return LabelTag{text}; }

// DateTimeMode tag: picks HTML5 date / time / datetime-local input variant
// on the frontend. Default is "datetime" if omitted.
struct DateTimeModeTag { const char* mode; };
inline DateTimeModeTag dateOnly() { return DateTimeModeTag{"date"}; }
inline DateTimeModeTag timeOnly() { return DateTimeModeTag{"time"}; }
inline DateTimeModeTag dateTime() { return DateTimeModeTag{"datetime"}; }

// ActionField: URL target, HTTP method, optional confirm text, optional MUI
// color variant (primary|secondary|error|warning|success|info|inherit).
struct ActionUrl    { const char* url; };
struct ActionMethod { const char* method; };
struct ActionConfirm { const char* text; };
struct ActionColor  { const char* color; };
inline ActionUrl     httpUrl(const char* u)     { return ActionUrl{u}; }
inline ActionMethod  httpMethod(const char* m)  { return ActionMethod{m}; }
inline ActionConfirm confirm(const char* t)     { return ActionConfirm{t}; }
inline ActionColor   color(const char* c)       { return ActionColor{c}; }

// MessageField level: MUI Alert severity (info|warning|error|success).
struct MessageLevel { const char* level; };
inline MessageLevel level(const char* l) { return MessageLevel{l}; }

// ActionField reference — points to a WebManager-registered action by id.
// Frontend resolves URL/method/confirm/color/icon/title/successMessage from
// the manifest action entry; ActionField has no direct URL then.
struct ActionRef { const char* id; };
inline ActionRef actionRef(const char* id) { return ActionRef{id}; }

// Display unit appended to the rendered value (e.g. "bytes", "MHz", "°C").
// Works for any field whose component shows a value; currently consumed by
// NumberField / TextField read-only renderings.
struct UnitTag { const char* text; };
inline UnitTag unit(const char* t) { return UnitTag{t}; }

// Conditional visibility — hide this field unless another field in the same
// form has the expected value. Frontend compares stringified values, so
// showIf("static_ip_config","true") matches a boolean true as well as the
// literal string "true".
struct ShowIfTag { const char* depField; const char* depValue; };
inline ShowIfTag showIf(const char* depField, const char* depValue) { return ShowIfTag{depField, depValue}; }

// Table row-click → fill target fields. Spec syntax:
//   "<target>=<col>,<target>=<col>,..."   — each pair means "on row click,
//                                             set form field <target> to the
//                                             clicked row's <col> value"
//   "<target>="                           — empty col clears <target> to ""
// Example: rowFill("ssid=ssid,password=") → click a scanned network row
//   to copy its ssid column into the ssid field and clear the password.
struct RowFillTag { const char* spec; };
inline RowFillTag rowFill(const char* spec) { return RowFillTag{spec}; }

// Smart-slot fallback for row-click. When set together with rowFill, the
// frontend inspects the CURRENT value of the FIRST target in `rowFill`
// (e.g. "ssid"). If that field is empty → use the primary rowFill spec.
// If filled AND the first target of rowFillFallback is empty → silently
// use the fallback (e.g. write to "ssid_2" instead of "ssid"). If both
// are filled → a Material dialog asks the operator which slot to
// overwrite. Same syntax as rowFill.
// Example: rowFill("ssid=ssid,pwd="), rowFillFallback("ssid=ssid_2,pwd=")
//   click a scanned row → primary if free, secondary if free, dialog if both.
struct RowFillFallbackTag { const char* spec; };
inline RowFillFallbackTag rowFillFallback(const char* spec) { return RowFillFallbackTag{spec}; }

// Table row-click → navigate to another route and carry the rowFill values
// through React Router location state as `state.dynamicPrefill`. A tab-local
// rowFill only updates the CURRENT tab's form state; rowNav lets a scan
// table on tab A prefill the settings form on tab B (e.g. scanner row →
// navigate("../settings", { state: { dynamicPrefill: {ssid:"X",password:""} } })).
// The target page's DynamicSettings picks up state.dynamicPrefill on mount.
struct RowNavTag { const char* path; };
inline RowNavTag rowNav(const char* path) { return RowNavTag{path}; }

// Table field rich-card mode. By default TABLE renders a data grid; adding
// layout("cards") switches it to a per-row card list where each row is a
// ListItem with Avatar + primary/secondary + trailing Badge, and clicking
// dispatches rowFill / rowNav like the grid mode. Use with TABLE:
//   addTableField(fields, "networks", AF::R,
//     col("ssid","SSID","text"), col("rssi","RSSI","number"),
//     col("channel","Ch","number"), col("security","Security","text"),
//     layout("cards"),
//     primaryField("ssid"),
//     secondaryTemplate("Security: {security}, Ch: {channel}"),
//     avatarFromField("icon"),
//     badgeField("rssi", "dBm", "Wifi"),
//     rowFill("ssid=ssid,password="), rowNav("../settings"));
struct TableLayoutTag { const char* mode; };   // "grid" (default) | "cards"
struct LCPrimaryTag   { const char* field; };
struct LCSecondaryTag { const char* tmpl;  };  // "Security: {security}, Ch: {channel}"
struct LCAvatarTag    { const char* field; };  // row[field] resolves to icon name
struct LCBadgeTag     { const char* field; const char* unit; const char* iconName; };

inline TableLayoutTag layout(const char* m)                                      { return {m}; }
inline LCPrimaryTag   primaryField(const char* f)                                { return {f}; }
inline LCSecondaryTag secondaryTemplate(const char* t)                           { return {t}; }
inline LCAvatarTag    avatarFromField(const char* f)                             { return {f}; }
inline LCBadgeTag     badgeField(const char* f, const char* unit, const char* ic){ return {f, unit, ic}; }

// Live color mapping — any text/number read-only field can carry a value→
// color map so its Avatar background follows state transitions coming over
// WS. Syntax: colorMap("Connected:success,Connecting:warning,default:info")
// with an optional "default:<name>" entry as fallback. Values are matched
// against the field's current rendered string.
struct ColorMapTag { const char* spec; };
inline ColorMapTag colorMap(const char* s) { return ColorMapTag{s}; }

// Universal avatar/label suppressor — when set, the rendering component
// skips its leading <Avatar> decoration (and the accompanying primary
// label in some widgets), showing just the raw value/body. Handy for
// feed-style lists (e.g. WiFi scan results) where the outer "Networks /
// no items" chrome is noise. Empty-state handling shrinks to nothing.
struct HideAvatarTag { bool on; };
inline HideAvatarTag hideAvatar(bool on = true) { return HideAvatarTag{on}; }

// ActionField: after a successful POST/GET/DELETE/PUT, fire a short
// polling schedule (500 / 2000 / 5000 / 8000 ms) of form refetches so
// UIs that need to surface backend-side progress (e.g. WiFi scan
// results dropping into a TABLE field) update automatically without
// the user having to reopen the tab. Requires the enclosing
// DynamicTabPanel — any tab rendered via DynamicFeature qualifies.
struct RefetchFormTag { bool on; };
inline RefetchFormTag refetchForm(bool on = true) { return RefetchFormTag{on}; }

// Universal "is-loading" indicator — widgets that render an empty state
// consult this tag against the current form state and show a spinner
// instead of the empty placeholder when the referenced field matches
// the given prefix. Example: loadingIf("scan_status:Scanning") on a
// networks table shows a CircularProgress while the scan is in flight
// (WiFi.scanComplete() == -1 → scan_status = "Scanning…").
struct LoadingIfTag { const char* spec; };  // "<field>:<valuePrefix>"
inline LoadingIfTag loadingIf(const char* s) { return LoadingIfTag{s}; }

// Card group wrapper — any number of existing fields (text, password, ip,
// number, …) can carry a `card("<groupId>")` tag. The frontend's
// DynamicContentHandler groups consecutive fields sharing the same groupId
// into one MUI <Card>, rendering each inner field as usual inside its
// <CardContent>. Decorate the FIRST field in the group with:
//   cardTitle("<text>")    — card header title
//   cardDeletable()        — adds an IconButton in the header that clears
//                            every field in the group (used by the WiFi
//                            Settings tab to "delete" a saved AP slot)
struct CardTag         { const char* groupId; };
struct CardTitleTag    { const char* title; };
struct CardDeletableTag{ bool on; };

inline CardTag          card(const char* id)              { return CardTag{id}; }
inline CardTitleTag     cardTitle(const char* t)          { return CardTitleTag{t}; }
inline CardDeletableTag cardDeletable(bool on = true)     { return CardDeletableTag{on}; }

// ====================== TABLE column declarations ======================
// One per column: key (payload field name), label (header text), type
// (text/number/boolean), optional format for number columns.
struct TableColumn {
  const char* key;
  const char* label;
  const char* type;
  const char* format;
};
inline TableColumn col(const char* key, const char* type) {
  return TableColumn{key, key, type, nullptr};
}
inline TableColumn col(const char* key, const char* label, const char* type) {
  return TableColumn{key, label, type, nullptr};
}
inline TableColumn col(const char* key, const char* label, const char* type, Format fmt) {
  return TableColumn{key, label, type, fmt.f};
}

static void parseTableColArg(String& out, const TableColumn& c) {
  if (out.length() > 0) out += ",";
  out += c.key;
  out += "|label="; out += (c.label ? c.label : c.key);
  out += "|type=";  out += (c.type ? c.type : "text");
  if (c.format && c.format[0] != '\0') { out += "|fmt="; out += c.format; }
}
template <typename T>
static void parseTableColArg(String&, T) {}  // ignore unknown
static void parseTableColArgs(String&) {}
template <typename First, typename... Rest>
static void parseTableColArgs(String& out, First first, Rest... rest) {
  parseTableColArg(out, first);
  parseTableColArgs(out, rest...);
}

// ====================== Парсер варіативних TREND-аргументів  ======================
static void parseLineArg(String& out, const TrendLine& ln) {
  if (out.length() > 0) out += ";";
  out += ln.keyName;
  out += ":";
  if (ln.hidden) out += "hidden=true,";
  out += "color="; out += ln.color;
  out += ",type="; out += ln.type;
}
static void parseLineArgs(String&) {}
template <typename First, typename... Rest>
static void parseLineArgs(String& out, First first, Rest... rest) {
  parseLineArg(out, first);
  parseLineArgs(out, rest...);
}
template <typename... Args>
static TrendLines lines(Args... lineObjs) {
  static String buffer;
  buffer = "";
  parseLineArgs(buffer, lineObjs...);
  return TrendLines{buffer.c_str()};
}

// ====================== Перелік типів поля ======================
enum class AF { R, RW };
inline const char* toString(AF af) {
  switch (af) { case AF::R: return "r"; case AF::RW: return "rw"; }
  return "r";
}

enum class FieldType {
  TEXT, NUMBER, SLIDER, CHECKBOX, SWITCH, DROPDOWN, TEXTAREA, RADIO, TREND, BUTTON, FILES, UPLOAD, TABLE, DATETIME, ACTION, MESSAGE, TIMEZONES, IP_ADDRESS, SECRET, UNKNOWN
};
inline const char* toString(FieldType ft) {
  switch (ft) {
    case FieldType::TEXT:       return "text";
    case FieldType::NUMBER:     return "number";
    case FieldType::SLIDER:     return "slider";
    case FieldType::CHECKBOX:   return "checkbox";
    case FieldType::SWITCH:     return "switch";
    case FieldType::DROPDOWN:   return "dropdown";
    case FieldType::TEXTAREA:   return "textarea";
    case FieldType::RADIO:      return "radio";
    case FieldType::TREND:      return "trend";
    case FieldType::BUTTON:     return "button";
    case FieldType::FILES:      return "files";
    case FieldType::UPLOAD:     return "upload";
    case FieldType::TABLE:      return "table";
    case FieldType::DATETIME:   return "datetime";
    case FieldType::ACTION:     return "action";
    case FieldType::MESSAGE:    return "message";
    case FieldType::TIMEZONES:  return "timezones";
    case FieldType::IP_ADDRESS: return "ip";
    case FieldType::SECRET:     return "secret";
    case FieldType::UNKNOWN:    return "unknown";
  }
  return "unknown";
}

// ====================== Парсер для number field ======================
struct MinVal;  // forward
static void parseNumberArg(MinVal m, double& mn, double&, const char*&) { mn = m.v; }
struct MaxVal;  // forward
static void parseNumberArg(MaxVal m, double&, double& mx, const char*&) { mx = m.v; }
struct Format;  // forward
static void parseNumberArg(Format f, double&, double&, const char*& fmt) { fmt = f.f; }
// Catch-all: IconName та будь-які інші «чужі» теги ігноруються тут —
// applyExtras() пройдеться по пакету окремо вкінці add*Field.
template <typename T>
static void parseNumberArg(T, double&, double&, const char*&) {}

static void parseNumberArgs(double&, double&, const char*&) {}
template <typename Arg>
static void parseNumberArgs(double& mn, double& mx, const char*& fmt, Arg arg) {
  parseNumberArg(arg, mn, mx, fmt);
  parseNumberArgs(mn, mx, fmt);
}
template <typename Arg, typename... Args>
static void parseNumberArgs(double& mn, double& mx, const char*& fmt, Arg arg, Args... rest) {
  parseNumberArg(arg, mn, mx, fmt);
  parseNumberArgs(mn, mx, fmt, rest...);
}

// ====================== Парсер для SLIDER (варіативні теги) ======================
static void parseSliderArg(MinVal m, double& mn, double&, double&, const char*&) { mn = m.v; }
static void parseSliderArg(MaxVal m, double&, double& mx, double&, const char*&) { mx = m.v; }
static void parseSliderArg(Step s, double&, double&, double& st, const char*&) { st = s.v; }
static void parseSliderArg(Placeholder p, double&, double&, double&, const char*& pl) { pl = p.t; }
template <typename T>
static void parseSliderArg(T, double&, double&, double&, const char*&) {}  // ignore unknown (IconName etc.)
static void parseSliderArgs(double&, double&, double&, const char*&) {}
template <typename Arg>
static void parseSliderArgs(double& mn, double& mx, double& st, const char*& pl, Arg arg) {
  parseSliderArg(arg, mn, mx, st, pl);
  parseSliderArgs(mn, mx, st, pl);
}
template <typename Arg, typename... Args>
static void parseSliderArgs(double& mn, double& mx, double& st, const char*& pl, Arg arg, Args... rest) {
  parseSliderArg(arg, mn, mx, st, pl);
  parseSliderArgs(mn, mx, st, pl, rest...);
}

// ====================== Парсер для BUTTON (опційно тільки placeholder) ======================
static void parseButtonArg(Placeholder p, const char*& pl) { pl = p.t; }
template <typename T>
static void parseButtonArg(T, const char*&) {}  // ignore unknown
static void parseButtonArgs(const char*&) {}
template <typename Arg>
static void parseButtonArgs(const char*& pl, Arg arg) {
  parseButtonArg(arg, pl);
  parseButtonArgs(pl);
}
template <typename Arg, typename... Args>
static void parseButtonArgs(const char*& pl, Arg arg, Args... rest) {
  parseButtonArg(arg, pl);
  parseButtonArgs(pl, rest...);
}

// ====================== Парсер для dropdown/radio options ======================
static void parseOptionsArg(Opt o, std::vector<Opt>& opts) { opts.push_back(o); }
template <typename T>
static void parseOptionsArg(T, std::vector<Opt>&) {}  // ignore unknown (IconName, placeholder, …)
static void parseOptionsArgs(std::vector<Opt>&) {}
template <typename Arg, typename... Args>
static void parseOptionsArgs(std::vector<Opt>& opts, Arg arg, Args... rest) {
  parseOptionsArg(arg, opts);
  parseOptionsArgs(opts, rest...);
}

// ====================== Парсер для TREND (варіативні теги) ======================
static void parseTrendArg(TrendXAxis t, String& xAxisKey, String&, bool&, bool&, int&, String&) {
  xAxisKey = t.xAxisKey;
}
static void parseTrendArg(TrendLines l, String&, String& linesStr, bool&, bool&, int&, String&) {
  linesStr = l.config;
}
static void parseTrendArg(TrendLegend lg, String&, String&, bool& showLegend, bool&, int&, String&) {
  showLegend = lg.show;
}
static void parseTrendArg(TrendTooltip tt, String&, String&, bool&, bool& showTooltip, int&, String&) {
  showTooltip = tt.show;
}
static void parseTrendArg(TrendMaxPoints p, String&, String&, bool&, bool&, int& maxPoints, String&) {
  maxPoints = p.value;
}
static void parseTrendArg(TrendMode m, String&, String&, bool&, bool&, int&, String& modeStr) {
  modeStr = m.mode;
}
template <typename T>
static void parseTrendArg(T, String&, String&, bool&, bool&, int&, String&) {}  // ignore unknown

static void parseTrendArgs(String&, String&, bool&, bool&, int&, String&) {}
template <typename Arg, typename... Args>
static void parseTrendArgs(String& xAxisKey, String& linesStr, bool& showLegend, bool& showTooltip, int& maxPoints, String& modeStr, Arg arg, Args... rest) {
  parseTrendArg(arg, xAxisKey, linesStr, showLegend, showTooltip, maxPoints, modeStr);
  parseTrendArgs(xAxisKey, linesStr, showLegend, showTooltip, maxPoints, modeStr, rest...);
}

// ====================== FormBuilder ======================
class FormBuilder {
 public:
  static JsonArray createForm(JsonObject& root, const char* name, const char* description) {
    JsonObject form = root.createNestedObject(name);
    form["description"] = description;
    return form.createNestedArray("fields");
  }

  // Загальний оновлювач (для небулевих типів)
  template <typename T>
  static bool updateValue(JsonObject& root, const char* key, T& value) {
    if (root.containsKey(key) && root[key].is<T>()) {
      T newValue = root[key].as<T>();
      if (newValue != value) { value = newValue; return true; }
    }
    return false;
  }

  // Спеціальний оновлювач для bool — приймаємо ТІЛЬКИ справжній JSON bool
  static bool updateValue(JsonObject& root, const char* key, bool& value) {
    if (root.containsKey(key) && root[key].is<bool>()) {
      bool nv = root[key].as<bool>();
      if (nv != value) { value = nv; return true; }
    }
    return false;
  }

   // uint8_t (години, хвилини, тощо)
  static bool updateValue(JsonObject& root, const char* key, uint8_t& value) {
    if (!root.containsKey(key)) return false;
    JsonVariant v = root[key];

    // ArduinoJson дозволяє as<double>() навіть із рядка "9.00"
    double d = v.as<double>();

    if (d < 0)   d = 0;
    if (d > 255) d = 255;

    uint8_t nv = static_cast<uint8_t>(d + 0.5);  // округлення
    if (nv != value) {
      value = nv;
      return true;
    }
    return false;
  }

  // uint16_t (типові мілісекунди, тощо)
  static bool updateValue(JsonObject& root, const char* key, uint16_t& value) {
    if (!root.containsKey(key)) return false;
    JsonVariant v = root[key];

    double d = v.as<double>();
    if (d < 0)      d = 0;
    if (d > 65535)  d = 65535;

    uint16_t nv = static_cast<uint16_t>(d + 0.5);
    if (nv != value) {
      value = nv;
      return true;
    }
    return false;
  }

  // int
  static bool updateValue(JsonObject& root, const char* key, int& value) {
    if (!root.containsKey(key)) return false;
    JsonVariant v = root[key];

    double d = v.as<double>();
    long   li = static_cast<long>(d >= 0 ? d + 0.5 : d - 0.5);

    int nv = static_cast<int>(li);
    if (nv != value) {
      value = nv;
      return true;
    }
    return false;
  }

  // float
  static bool updateValue(JsonObject& root, const char* key, float& value) {
    if (!root.containsKey(key)) return false;
    JsonVariant v = root[key];

    float nv = v.as<float>();  // і з числа, і з рядка працює
    if (nv != value) {
      value = nv;
      return true;
    }
    return false;
  }

  // double
  static bool updateValue(JsonObject& root, const char* key, double& value) {
    if (!root.containsKey(key)) return false;
    JsonVariant v = root[key];

    double nv = v.as<double>();
    if (nv != value) {
      value = nv;
      return true;
    }
    return false;
  }

  // ---------- TEXT ----------
  template <typename... Extras>
  static JsonObject addTextField(JsonArray& fields, const char* key, AF accessFlag, const char* val, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    // ArduinoJson stores raw const char* as a LINKED string (zero-copy).
    // Wrap into String so the document OWNS the bytes — safe for callers
    // that pass c_str() on a local String that goes out of scope before
    // the response is serialised.
    field[key] = String(val ? val : "");
    setBasicOptions(field, FieldType::TEXT, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- SECRET ----------
  // Single declaration for password / token / signing-key fields.
  // FRONTEND: routed to a dedicated `SecretField.tsx` element that
  // renders type=password + eye-toggle. NOT a TextField with a flag —
  // a parallel element so it can grow secret-specific affordances
  // (rotate, generate, copy) without polluting TextField.
  // BACKEND: ConfigManager probes the form schema at registration and
  // auto-populates `ConfigDescriptor::secretKeys` with every key whose
  // option string starts with "secret;". When FT_SECRETS_VAULT=1 those
  // keys are AES-128-CBC encrypted on disk via the chip-derived key.
  // Service authors NEVER write a `secretKeys` list manually.
  //
  // Probe-mode escape hatch: if a future buildForm needs runtime state
  // to even SHAPE the schema, gate the relevant section on
  // !ConfigManager::isProbing() — SECRET fields themselves should
  // ALWAYS be emitted because their structure is what discovery hunts.
  template <typename... Extras>
  static JsonObject addSecretField(JsonArray& fields, const char* key, AF accessFlag, const char* val, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(val ? val : "");
    setBasicOptions(field, FieldType::SECRET, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- NUMBER ----------
  template <typename... Args>
  static JsonObject addNumberField(JsonArray& fields, const char* key, AF accessFlag, double value, Args... numberArgs) {
    double mn = 0, mx = 100; const char* fmt = "";
    parseNumberArgs(mn, mx, fmt, numberArgs...);

    // Derive decimal count from fmt string: "0" → 0, "0.0" → 1, "0.00" → 2, …
    int decimals = 0;
    if (fmt && fmt[0] != '\0') {
      const char* dot = strchr(fmt, '.');
      if (dot) {
        const char* p = dot + 1;
        while (*p == '0') { ++decimals; ++p; }
      }
    }

    JsonObject field = fields.createNestedObject();
    field[key] = String(value, decimals);  // stringified with proper precision

    String oValue = String(toString(FieldType::NUMBER)) + ";" + toString(accessFlag);
    oValue += ";mn="; oValue += mn;
    oValue += ";mx="; oValue += mx;
    if (fmt && fmt[0] != '\0') { oValue += ";f="; oValue += fmt; }

    field["o"] = oValue;
    applyExtras(field, numberArgs...);
    return field;
  }

  // ---------- SLIDER ----------
  template <typename... Args>
  static JsonObject addSliderField(JsonArray& fields, const char* key, AF accessFlag, double value, Args... sliderArgs) {
    double mn = 0, mx = 100, st = 1; const char* pl = nullptr;
    parseSliderArgs(mn, mx, st, pl, sliderArgs...);

    JsonObject field = fields.createNestedObject();
    field[key] = value; // числом

    String oValue = String(toString(FieldType::SLIDER)) + ";" + toString(accessFlag);
    oValue += ";mn="; oValue += mn;
    oValue += ";mx="; oValue += mx;
    if (!std::isnan(st)) { oValue += ";st="; oValue += st; }
    if (pl && pl[0] != '\0') { oValue += ";pl="; oValue += pl; }

    field["o"] = oValue;
    applyExtras(field, sliderArgs...);
    return field;
  }

  // ---------- CHECKBOX (boolean) ----------
  template <typename... Extras>
  static JsonObject addCheckboxField(JsonArray& fields, const char* key, AF accessFlag, bool value, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = value;  // ЛИШЕ true/false
    setBasicOptions(field, FieldType::CHECKBOX, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- SWITCH (boolean) ----------
  template <typename... Extras>
  static JsonObject addSwitchField(JsonArray& fields, const char* key, AF accessFlag, bool value, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = value;  // ЛИШЕ true/false
    setBasicOptions(field, FieldType::SWITCH, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- BUTTON (boolean) ----------
  template <typename... Args>
  static JsonObject addButtonField(JsonArray& fields, const char* key, AF accessFlag, bool value, Args... btnArgs) {
    const char* pl = nullptr;
    parseButtonArgs(pl, btnArgs...);

    JsonObject field = fields.createNestedObject();
    field[key] = value;  // ЛИШЕ true/false

    String oValue = String(toString(FieldType::BUTTON)) + ";" + toString(accessFlag);
    if (pl && pl[0] != '\0') { oValue += ";pl="; oValue += pl; }

    field["o"] = oValue;
    applyExtras(field, btnArgs...);
    return field;
  }

  // ---------- DROPDOWN ----------
  template <typename... Args>
  static JsonObject addDropdownField(JsonArray& fields, const char* key, AF accessFlag, int selectedValue, Args... optArgs) {
    JsonObject field = fields.createNestedObject();
    field[key] = selectedValue;

    std::vector<Opt> options;
    parseOptionsArgs(options, optArgs...);

    String oValue = String(toString(FieldType::DROPDOWN)) + ";" + toString(accessFlag);
    if (!options.empty()) {
      oValue += ";options=";
      bool first = true;
      for (auto& op : options) {
        if (!first) oValue += ",";
        // Emit "label:value" so the frontend can round-trip the explicit int
        // instead of deriving it from a 1-based index. Kept backward-compatible:
        // a consumer that splits on ',' still sees the label prefix and can
        // fall back to index ordering.
        oValue += op.label;
        oValue += ":";
        oValue += op.value;
        first = false;
      }
    }

    field["o"] = oValue;
    applyExtras(field, optArgs...);
    return field;
  }

  // ---------- TEXTAREA ----------
  template <typename... Extras>
  static JsonObject addTextareaField(JsonArray& fields, const char* key, AF accessFlag, const String& value, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = value;
    setBasicOptions(field, FieldType::TEXTAREA, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- RADIO ----------
  template <typename... Args>
  static JsonObject addRadioField(JsonArray& fields, const char* key, AF accessFlag, int selectedValue, Args... optArgs) {
    JsonObject field = fields.createNestedObject();
    field[key] = selectedValue;

    std::vector<Opt> options;
    parseOptionsArgs(options, optArgs...);

    String oValue = String(toString(FieldType::RADIO)) + ";" + toString(accessFlag);
    if (!options.empty()) {
      oValue += ";options=";
      bool first = true;
      for (auto& op : options) {
        if (!first) oValue += ",";
        oValue += op.label;
        oValue += ":";
        oValue += op.value;
        first = false;
      }
    }

    field["o"] = oValue;
    applyExtras(field, optArgs...);
    return field;
  }

  // ---------- TREND ----------
  template <typename... Args>
  static JsonObject addTrendField(JsonArray& fields, const char* key, AF accessFlag, Args... trendArgs) {
    JsonObject field = fields.createNestedObject();
    JsonObject trendObj = field.createNestedObject(key); (void)trendObj; // зарезервовано для сумісності

    String xAxisKeyStr = "timestamp";
    String linesStr;
    bool showLegend = false, showTooltip = false;
    int maxPts = 100;
    String modeStr = "lineChart";

    parseTrendArgs(xAxisKeyStr, linesStr, showLegend, showTooltip, maxPts, modeStr, trendArgs...);

    String oValue = String(toString(FieldType::TREND)) + ";" + toString(accessFlag);
    oValue += ";mode=";  oValue += modeStr;
    if (modeStr == "barChart") { oValue += ";xAxis=keys"; }
    else { oValue += ";xAxis="; oValue += xAxisKeyStr; }
    if (linesStr.length() > 0) { oValue += ";lines="; oValue += linesStr; }
    if (showLegend)  oValue += ";legend=true";
    if (showTooltip) oValue += ";tooltip=true";
    if (maxPts > 0)  { oValue += ";maxPoints="; oValue += maxPts; }

    field["o"] = oValue;
    applyExtras(field, trendArgs...);
    return field;
  }

  // ---------- FILES (interactive file browser widget) ----------
  // The field's value is the initial path ("/flash" by default). The `o` string carries:
  //   files;<af>;base=<rest base>[;initial=<path>][;uploadBase=<path>]
  // The frontend FilesField component self-manages navigation/actions against those URLs.
  template <typename... Extras>
  static JsonObject addFilesField(JsonArray& fields,
                                  const char* key,
                                  AF accessFlag,
                                  const char* restBase,
                                  const char* initialPath = "/flash",
                                  const char* uploadBase = nullptr,
                                  Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(initialPath ? initialPath : "/flash");

    String oValue = String(toString(FieldType::FILES)) + ";" + toString(accessFlag);
    if (restBase && restBase[0] != '\0') {
      oValue += ";base="; oValue += restBase;
    }
    if (initialPath && initialPath[0] != '\0') {
      oValue += ";initial="; oValue += initialPath;
    }
    if (uploadBase && uploadBase[0] != '\0') {
      oValue += ";uploadBase="; oValue += uploadBase;
    }
    field["o"] = oValue;
    applyExtras(field, extras...);
    return field;
  }

  // ---------- UPLOAD (drag-drop upload widget) ----------
  //   upload;<af>;url=<upload endpoint>[;accept=<mime/ext>]
  template <typename... Extras>
  static JsonObject addUploadField(JsonArray& fields,
                                   const char* key,
                                   AF accessFlag,
                                   const char* uploadUrl,
                                   const char* accept = nullptr,
                                   Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = "";

    String oValue = String(toString(FieldType::UPLOAD)) + ";" + toString(accessFlag);
    if (uploadUrl && uploadUrl[0] != '\0') {
      oValue += ";url="; oValue += uploadUrl;
    }
    if (accept && accept[0] != '\0') {
      oValue += ";accept="; oValue += accept;
    }
    field["o"] = oValue;
    applyExtras(field, extras...);
    return field;
  }

  // ---------- ACTION (one-shot button that fires HTTP call) ----------
  //   action;rw;url=<target>[;method=POST|GET|DELETE|…][;confirm=<text>][;color=<muiColor>][;icon=…]
  // The field's value is the displayed button label. Click flow on the
  // frontend: optional confirmation dialog → axios request to url with the
  // configured method → snackbar on success/failure. Default method POST.
  template <typename... Extras>
  static JsonObject addActionField(JsonArray& fields, const char* key, const char* label,
                                   AF accessFlag, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(label ? label : key);  // copy — safe for local labels
    setBasicOptions(field, FieldType::ACTION, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- MESSAGE (static MUI Alert) ----------
  //   message;r;level=<info|warning|error|success>[;icon=…]
  // Field value is the text body. level() picks Alert severity (default info).
  template <typename... Extras>
  static JsonObject addMessageField(JsonArray& fields, const char* key, const char* text,
                                    Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(text ? text : "");  // copy — message text may be dynamic
    setBasicOptions(field, FieldType::MESSAGE, AF::R);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- DATETIME (HTML5 date / time / datetime-local) ----------
  //   datetime;<af>[;dtmode=<date|time|datetime>][;icon=...]
  // Value is Unix epoch seconds. The frontend picks the HTML input variant
  // from dtmode (default "datetime"), formats seconds → local ISO string,
  // and emits seconds back on change. Range is int32_t (sufficient until
  // 2038 and well past any practical device-lifetime window).
  template <typename... Extras>
  static JsonObject addDateTimeField(JsonArray& fields, const char* key, AF accessFlag,
                                     int32_t valueSeconds, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = valueSeconds;
    setBasicOptions(field, FieldType::DATETIME, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- TABLE (MUI data grid, WS-driven rows) ----------
  //   table;<af>;cols=<col1>|label=..|type=..[|fmt=..],<col2>|...
  // Schema-only: the field slot is initialised to an empty array. Runtime
  // rows come via WS readSta (or a subsequent REST emission) — the frontend
  // replaces the full row set on every WS push under this field's key.
  template <typename... Args>
  static JsonObject addTableField(JsonArray& fields, const char* key, AF accessFlag, Args... columns) {
    JsonObject field = fields.createNestedObject();
    field.createNestedArray(key);  // initial value = []

    String colsStr;
    parseTableColArgs(colsStr, columns...);

    String oValue = String(toString(FieldType::TABLE)) + ";" + toString(accessFlag);
    if (colsStr.length() > 0) { oValue += ";cols="; oValue += colsStr; }
    field["o"] = oValue;
    applyExtras(field, columns...);
    return field;
  }

  // ---------- IP ADDRESS (dotted-quad with frontend validation) ----------
  // Emits a plain string; the frontend renders a TextField with an IPv4
  // regex validator so malformed input is flagged before submission.
  template <typename... Extras>
  static JsonObject addIPAddressField(JsonArray& fields, const char* key, AF accessFlag,
                                      const char* value, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(value ? value : "");
    setBasicOptions(field, FieldType::IP_ADDRESS, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- TIMEZONES (frontend-owned dropdown) ----------
  // The list of ~400 IANA tz labels lives only on the frontend. Backend emits
  // the current label string; on change the widget also mutates a sibling
  // field (conventionally `tz_format`) with the POSIX TZ string.
  template <typename... Extras>
  static JsonObject addTimeZonesField(JsonArray& fields, const char* key, AF accessFlag,
                                      const char* label, Extras... extras) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(label ? label : "");
    setBasicOptions(field, FieldType::TIMEZONES, accessFlag);
    applyExtras(field, extras...);
    return field;
  }

  // ---------- DEFAULT ----------
  static JsonObject addDefaultField(JsonArray& fields, const char* key, AF accessFlag, const char* value) {
    JsonObject field = fields.createNestedObject();
    field[key] = String(value ? value : "");
    setBasicOptions(field, FieldType::UNKNOWN, accessFlag);
    return field;
  }

 private:
  // ------- Universal extras pipeline -------
  // Кожен add*Field прокидає власний variadic pack через applyExtras(field, ...).
  // applyExtras() вибирає з пакету знайомі теги (наразі IconName; далі легко
  // додавати Hint, Color тощо) і модифікує field["o"] за ними. Невідомі теги
  // мовчки ігноруються — так-що поле може приймати як свої доменні теги
  // (MinVal, MaxVal, Opt…), так і наскрізні (IconName) в довільному порядку.
  static void applyExtra(JsonObject& field, IconName ic) {
    if (ic.name && ic.name[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";icon="; o += ic.name;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, LabelTag lab) {
    if (lab.text && lab.text[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";l="; o += lab.text;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, DateTimeModeTag m) {
    if (m.mode && m.mode[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";dtmode="; o += m.mode;
      field["o"] = o;
    }
  }
  // Trend bakes its own "mode" into oValue via parseTrendArgs — don't also
  // emit it here or we'd duplicate the key. Tables use a separate TableMode
  // tag so there's no collision.
  static void applyExtra(JsonObject& /*field*/, TrendMode /*m*/) {}
  static void applyExtra(JsonObject& field, TableMode m) {
    if (m.mode && m.mode[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";mode="; o += m.mode;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, TableMaxRows m) {
    if (m.v > 0) {
      String o = field["o"].as<String>();
      o += ";maxRows="; o += m.v;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ActionUrl a) {
    if (a.url && a.url[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";url="; o += a.url;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ActionMethod m) {
    if (m.method && m.method[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";method="; o += m.method;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ActionConfirm c) {
    if (c.text && c.text[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";confirm="; o += c.text;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ActionColor c) {
    if (c.color && c.color[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";color="; o += c.color;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, MessageLevel l) {
    if (l.level && l.level[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";level="; o += l.level;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ActionRef ref) {
    if (ref.id && ref.id[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";ref="; o += ref.id;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, UnitTag u) {
    if (u.text && u.text[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";unit="; o += u.text;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ShowIfTag s) {
    if (s.depField && s.depField[0] != '\0' && s.depValue) {
      String o = field["o"].as<String>();
      o += ";showIf="; o += s.depField; o += ":"; o += s.depValue;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, RowFillTag r) {
    if (r.spec && r.spec[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";rowFill="; o += r.spec;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, RowFillFallbackTag r) {
    if (r.spec && r.spec[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";rowFillFallback="; o += r.spec;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, RowNavTag r) {
    if (r.path && r.path[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";rowNav="; o += r.path;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, TableLayoutTag l) {
    if (l.mode && l.mode[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";layout="; o += l.mode;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, LCPrimaryTag p) {
    if (p.field && p.field[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";primary="; o += p.field;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, LCSecondaryTag s) {
    if (s.tmpl && s.tmpl[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";secondary="; o += s.tmpl;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, LCAvatarTag a) {
    if (a.field && a.field[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";avatar="; o += a.field;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, LCBadgeTag b) {
    if (b.field && b.field[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";badge="; o += b.field;
      o += "|"; o += (b.unit ? b.unit : "");
      o += "|"; o += (b.iconName ? b.iconName : "");
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, ColorMapTag c) {
    if (c.spec && c.spec[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";colorMap="; o += c.spec;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, CardTag c) {
    if (c.groupId && c.groupId[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";card="; o += c.groupId;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, CardTitleTag c) {
    if (c.title && c.title[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";cardTitle="; o += c.title;
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, CardDeletableTag c) {
    if (c.on) {
      String o = field["o"].as<String>();
      o += ";cardDeletable=1";
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, HideAvatarTag h) {
    if (h.on) {
      String o = field["o"].as<String>();
      o += ";hideAvatar=1";
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, RefetchFormTag r) {
    if (r.on) {
      String o = field["o"].as<String>();
      o += ";refetchForm=1";
      field["o"] = o;
    }
  }
  static void applyExtra(JsonObject& field, LoadingIfTag l) {
    if (l.spec && l.spec[0] != '\0') {
      String o = field["o"].as<String>();
      o += ";loadingIf="; o += l.spec;
      field["o"] = o;
    }
  }
  template <typename T>
  static void applyExtra(JsonObject&, T) {}  // ignore unknown tags

  static void applyExtras(JsonObject&) {}
  template <typename First, typename... Rest>
  static void applyExtras(JsonObject& field, First first, Rest... rest) {
    applyExtra(field, first);
    applyExtras(field, rest...);
  }

  static void setBasicOptions(JsonObject& field, FieldType ft, AF af) {
    String oValue = String(toString(ft)) + ";" + toString(af);
    field["o"] = oValue;
  }
};

#endif  // FORMBUILDER_H
