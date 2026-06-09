#ifndef WEB_FORM_H
#define WEB_FORM_H

#include <string.h>

#include <ArduinoJson.h>

#include "FieldVocab.h"

// ─────────────────────────────────────────────────────────────────────────
//  Fluent form/field builders — the FormBuilder front door, same style as
//  WebFeature (web::WebFeature(...).menu(...).tab(...)). Authoring reads as:
//
//      web::Form form(root, "resources", "Device resources");
//      form.add(web::Text("name", s.name).label("Name").icon("Label"));
//      form.add(web::Number("cpu", 240).readOnly().min(0).max(500).unit("MHz"));
//      form.add(web::Dropdown("mode", s.mode).option("Auto", 0).option("Manual", 1));
//      form.add(web::Table("rows").col("ts", "Time", "number").mode("append").maxRows(50));
//      form.add(web::Trend("chart").line("sin", "#f00", "line").legend().maxPoints(100));
//
//  Every `.` autocompletes (member functions, not free tag-functions), and a
//  builder only exposes the options its field type actually supports — calling
//  .min() on a Text won't compile. Universal options (icon/label/showIf/card…)
//  live on the FieldSpec base, so every field has them.
//
//  Each builder is a plain value object; web::Form::add() materializes it into
//  the form's "fields" array, producing the SAME JSON the old
//  FormBuilder::add*Field emitted (verified per-conversion). The wire option
//  string is parsed order-independently on the frontend, so segment order is
//  free. All keys/types come from FieldVocab.h — no raw literals here.
// ─────────────────────────────────────────────────────────────────────────

namespace web {

// ── Universal base (CRTP) — options every field type accepts. ──
template <typename D>
class FieldSpec {
 public:
  explicit FieldSpec(const char* key) : _key(key) {}

  // Access flag.
  D& readOnly()  { _af = Access::Read;      return self(); }
  D& readWrite() { _af = Access::ReadWrite; return self(); }

  // Universal options (present in every field type's frontend option list).
  D& label(const char* t)       { return putStr(Opt::Label, t); }
  D& icon(const char* n)        { return putStr(Opt::Icon, n); }
  D& color(const char* c)       { return putStr(Opt::Color, c); }
  D& card(const char* groupId)  { return putStr(Opt::Card, groupId); }
  D& cardTitle(const char* t)   { return putStr(Opt::CardTitle, t); }
  D& cardDeletable(bool on = true) { return on ? putFlag1(Opt::CardDeletable) : self(); }
  D& hideAvatar(bool on = true)    { return on ? putFlag1(Opt::HideAvatar) : self(); }
  D& refetchForm(bool on = true)   { return on ? putFlag1(Opt::RefetchForm) : self(); }
  D& showIf(const char* depField, const char* depValue) {
    String s = depField; s += ':'; s += (depValue ? depValue : "");
    return putStr(Opt::ShowIf, s);
  }
  D& loadingIf(const char* spec)   { return putStr(Opt::LoadingIf, spec); }

 protected:
  const char* _key;
  const char* _af = Access::ReadWrite;
  String _opts;  // accumulated ";key=value" / ";flag" segments (order-free)

  D& self() { return static_cast<D&>(*this); }

  D& putStr(const char* k, const char* v) {
    if (v && v[0]) { _opts += ';'; _opts += k; _opts += '='; _opts += v; }
    return self();
  }
  D& putStr(const char* k, const String& v) {
    if (v.length()) { _opts += ';'; _opts += k; _opts += '='; _opts += v; }
    return self();
  }
  D& putReal(const char* k, double v) { _opts += ';'; _opts += k; _opts += '='; _opts += v; return self(); }
  D& putInt(const char* k, long v)    { _opts += ';'; _opts += k; _opts += '='; _opts += v; return self(); }
  D& putFlag1(const char* k)          { _opts += ';'; _opts += k; _opts += "=1"; return self(); }

  // "<type>;<af>" + accumulated universal/extra opts. Complex types insert
  // their own core segments (mn/mx, cols, options, lines) before _opts.
  String head(const char* type) const { String o = type; o += ';'; o += _af; return o; }
  const String& tail() const { return _opts; }

  // Default materializer for "simple" fields (value already a JSON scalar):
  // create the field object, leave value-setting to the caller, append o.
  JsonObject open(JsonArray& fields, const char* type) const {
    JsonObject f = fields.createNestedObject();
    f["o"] = head(type) + _opts;
    return f;
  }
};

// ── Simple text-ish fields ────────────────────────────────────────────────
class Text : public FieldSpec<Text> {
 public:
  Text(const char* key, const char* value) : FieldSpec(key), _v(value) {}
  Text& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Text& unit(const char* u)        { return putStr(Opt::Unit, u); }
  Text& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Text)[_key] = String(_v ? _v : ""); }
 private:
  const char* _v;
};

class Secret : public FieldSpec<Secret> {
 public:
  Secret(const char* key, const char* value) : FieldSpec(key), _v(value) {}
  Secret& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Secret& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Secret)[_key] = String(_v ? _v : ""); }
 private:
  const char* _v;
};

class Textarea : public FieldSpec<Textarea> {
 public:
  Textarea(const char* key, const String& value) : FieldSpec(key), _v(value) {}
  Textarea& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Textarea& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Textarea)[_key] = _v; }
 private:
  String _v;
};

class Ip : public FieldSpec<Ip> {
 public:
  Ip(const char* key, const char* value) : FieldSpec(key), _v(value) {}
  Ip& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Ip& unit(const char* u)        { return putStr(Opt::Unit, u); }
  Ip& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Ip)[_key] = String(_v ? _v : ""); }
 private:
  const char* _v;
};

class TimeZones : public FieldSpec<TimeZones> {
 public:
  TimeZones(const char* key, const char* label) : FieldSpec(key), _v(label) {}
  void writeTo(JsonArray& fields) const { open(fields, FieldType::TimeZones)[_key] = String(_v ? _v : ""); }
 private:
  const char* _v;
};

// ── Booleans ────────────────────────────────────────────────────────────--
class Checkbox : public FieldSpec<Checkbox> {
 public:
  Checkbox(const char* key, bool value) : FieldSpec(key), _v(value) {}
  Checkbox& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Checkbox& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Checkbox)[_key] = _v; }
 private:
  bool _v;
};

class Switch : public FieldSpec<Switch> {
 public:
  Switch(const char* key, bool value) : FieldSpec(key), _v(value) {}
  Switch& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Switch& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Switch)[_key] = _v; }
 private:
  bool _v;
};

class Button : public FieldSpec<Button> {
 public:
  Button(const char* key, bool value) : FieldSpec(key), _v(value) {}
  Button& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Button& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Button)[_key] = _v; }
 private:
  bool _v;
};

// ── Number / slider (core segments have defaults → emitted explicitly) ──
class Number : public FieldSpec<Number> {
 public:
  Number(const char* key, double value) : FieldSpec(key), _v(value) {}
  Number& min(double v)            { _min = v; return *this; }
  Number& max(double v)            { _max = v; return *this; }
  Number& format(const char* fmt)  { _fmt = fmt; return *this; }
  Number& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Number& unit(const char* u)        { return putStr(Opt::Unit, u); }
  Number& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const {
    int decimals = decimalsOf(_fmt);
    JsonObject f = fields.createNestedObject();
    f[_key] = String(_v, decimals);  // stringified at the schema's precision
    String o = head(FieldType::Number);
    o += ';'; o += Opt::Min; o += '='; o += _min;
    o += ';'; o += Opt::Max; o += '='; o += _max;
    if (_fmt && _fmt[0]) { o += ';'; o += Opt::Format; o += '='; o += _fmt; }
    o += tail();
    f["o"] = o;
  }
 private:
  double _v;
  double _min = 0, _max = 100;
  const char* _fmt = "";
  static int decimalsOf(const char* fmt) {
    int d = 0;
    if (fmt && fmt[0]) { const char* dot = strchr(fmt, '.'); if (dot) { const char* p = dot + 1; while (*p++ == '0') ++d; } }
    return d;
  }
};

class Slider : public FieldSpec<Slider> {
 public:
  Slider(const char* key, double value) : FieldSpec(key), _v(value) {}
  Slider& min(double v)  { _min = v; return *this; }
  Slider& max(double v)  { _max = v; return *this; }
  Slider& step(double v) { _step = v; return *this; }
  Slider& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Slider& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = _v;  // numeric
    String o = head(FieldType::Slider);
    o += ';'; o += Opt::Min; o += '='; o += _min;
    o += ';'; o += Opt::Max; o += '='; o += _max;
    o += ';'; o += Opt::Step; o += '='; o += _step;
    o += tail();
    f["o"] = o;
  }
 private:
  double _v;
  double _min = 0, _max = 100, _step = 1;
};

// ── Dropdown / radio (option list) ─────────────────────────────────────────
class Dropdown : public FieldSpec<Dropdown> {
 public:
  Dropdown(const char* key, int selected) : FieldSpec(key), _v(selected) {}
  Dropdown& option(const char* label, int value) {
    if (_options.length()) _options += ',';
    _options += label; _options += ':'; _options += value;
    return *this;
  }
  Dropdown& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Dropdown& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = _v;
    String o = head(FieldType::Dropdown);
    if (_options.length()) { o += ';'; o += Opt::Options; o += '='; o += _options; }
    o += tail();
    f["o"] = o;
  }
 private:
  int _v;
  String _options;
};

class Radio : public FieldSpec<Radio> {
 public:
  Radio(const char* key, int selected) : FieldSpec(key), _v(selected) {}
  Radio& option(const char* label, int value) {
    if (_options.length()) _options += ',';
    _options += label; _options += ':'; _options += value;
    return *this;
  }
  Radio& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Radio& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = _v;
    String o = head(FieldType::Radio);
    if (_options.length()) { o += ';'; o += Opt::Options; o += '='; o += _options; }
    o += tail();
    f["o"] = o;
  }
 private:
  int _v;
  String _options;
};

// ── Trend / chart ───────────────────────────────────────────────────────--
class Trend : public FieldSpec<Trend> {
 public:
  explicit Trend(const char* key) : FieldSpec(key) { _af = Access::Read; }
  Trend& mode(const char* m)      { _mode = m; return *this; }
  Trend& xAxis(const char* key_)  { _xAxis = key_; return *this; }
  Trend& maxPoints(int n)         { _maxPoints = n; return *this; }
  Trend& legend(bool on = true)   { _legend = on; return *this; }
  Trend& tooltip(bool on = true)  { _tooltip = on; return *this; }
  Trend& line(const char* key_, const char* color, const char* type) { return line(key_, false, color, type); }
  Trend& line(const char* key_, bool hidden, const char* color, const char* type) {
    if (_lines.length()) _lines += ';';
    _lines += key_; _lines += ':';
    if (hidden) _lines += "hidden=true,";
    _lines += "color="; _lines += color;
    _lines += ",type="; _lines += type;
    return *this;
  }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f.createNestedObject(_key);  // reserved value slot (compat)
    String o = head(FieldType::Trend);
    o += ';'; o += Opt::Mode; o += '='; o += _mode;
    o += ';'; o += Opt::TrendXAxis; o += '=';
    o += (String(_mode) == "barChart") ? "keys" : _xAxis;
    if (_lines.length())  { o += ';'; o += Opt::Lines; o += '='; o += _lines; }
    if (_legend)  { o += ';'; o += Opt::Legend;  o += "=true"; }
    if (_tooltip) { o += ';'; o += Opt::Tooltip; o += "=true"; }
    if (_maxPoints > 0) { o += ';'; o += Opt::MaxPoints; o += '='; o += _maxPoints; }
    o += tail();
    f["o"] = o;
  }
 private:
  const char* _mode = "lineChart";
  String _xAxis = "timestamp";
  String _lines;
  bool _legend = false, _tooltip = false;
  int _maxPoints = 100;
};

// ── Table (schema only; runtime rows stream via WS) ─────────────────────────
class Table : public FieldSpec<Table> {
 public:
  explicit Table(const char* key) : FieldSpec(key) {}
  Table& col(const char* key_, const char* type) { return col(key_, key_, type, nullptr); }
  Table& col(const char* key_, const char* label, const char* type) { return col(key_, label, type, nullptr); }
  Table& col(const char* key_, const char* label, const char* type, const char* fmt) {
    if (_cols.length()) _cols += ',';
    _cols += key_;
    _cols += "|label="; _cols += (label ? label : key_);
    _cols += "|type=";  _cols += (type ? type : "text");
    if (fmt && fmt[0]) { _cols += "|fmt="; _cols += fmt; }
    return *this;
  }
  Table& mode(const char* m)            { return putStr(Opt::Mode, m); }
  Table& maxRows(int n)                 { return putInt(Opt::MaxRows, n); }
  Table& rowFill(const char* spec)      { return putStr(Opt::RowFill, spec); }
  Table& rowFillFallback(const char* s) { return putStr(Opt::RowFillFallback, s); }
  Table& rowNav(const char* path)       { return putStr(Opt::RowNav, path); }
  Table& layout(const char* m)          { return putStr(Opt::Layout, m); }
  Table& primary(const char* field)     { return putStr(Opt::Primary, field); }
  Table& secondary(const char* tmpl)    { return putStr(Opt::Secondary, tmpl); }
  Table& avatar(const char* field)      { return putStr(Opt::Avatar, field); }
  Table& badge(const char* field, const char* unit, const char* iconName) {
    String s = field; s += '|'; s += (unit ? unit : ""); s += '|'; s += (iconName ? iconName : "");
    return putStr(Opt::Badge, s);
  }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f.createNestedArray(_key);  // initial value = []
    String o = head(FieldType::Table);
    if (_cols.length()) { o += ';'; o += Opt::Cols; o += '='; o += _cols; }
    o += tail();
    f["o"] = o;
  }
 private:
  String _cols;
};

// ── Files / upload ──────────────────────────────────────────────────────--
class Files : public FieldSpec<Files> {
 public:
  Files(const char* key, const char* restBase) : FieldSpec(key), _base(restBase) {}
  Files& initial(const char* path)    { _initial = path; return *this; }
  Files& uploadBase(const char* path) { _uploadBase = path; return *this; }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = String(_initial ? _initial : "/flash");
    String o = head(FieldType::Files);
    if (_base && _base[0])             { o += ';'; o += Opt::Base; o += '='; o += _base; }
    if (_initial && _initial[0])       { o += ';'; o += Opt::Initial; o += '='; o += _initial; }
    if (_uploadBase && _uploadBase[0]) { o += ';'; o += Opt::UploadBase; o += '='; o += _uploadBase; }
    o += tail();
    f["o"] = o;
  }
 private:
  const char* _base;
  const char* _initial = "/flash";
  const char* _uploadBase = nullptr;
};

class Upload : public FieldSpec<Upload> {
 public:
  Upload(const char* key, const char* url) : FieldSpec(key), _url(url) {}
  Upload& accept(const char* a) { _accept = a; return *this; }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = "";
    String o = head(FieldType::Upload);
    if (_url && _url[0])       { o += ';'; o += Opt::Url; o += '='; o += _url; }
    if (_accept && _accept[0]) { o += ';'; o += Opt::Accept; o += '='; o += _accept; }
    o += tail();
    f["o"] = o;
  }
 private:
  const char* _url;
  const char* _accept = nullptr;
};

// ── DateTime ────────────────────────────────────────────────────────────--
class DateTime : public FieldSpec<DateTime> {
 public:
  DateTime(const char* key, int32_t valueSeconds) : FieldSpec(key), _v(valueSeconds) {}
  DateTime& dateOnly() { return putStr(Opt::DateTimeMode, "date"); }
  DateTime& timeOnly() { return putStr(Opt::DateTimeMode, "time"); }
  DateTime& dateTime() { return putStr(Opt::DateTimeMode, "datetime"); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::DateTime)[_key] = _v; }
 private:
  int32_t _v;
};

// ── Action (button firing an HTTP call or referencing a manifest action) ──
class Action : public FieldSpec<Action> {
 public:
  Action(const char* key, const char* label = nullptr) : FieldSpec(key), _label(label) {}
  Action& url(const char* u)        { return putStr(Opt::Url, u); }
  Action& method(const char* m)     { return putStr(Opt::Method, m); }
  Action& confirm(const char* t)    { return putStr(Opt::Confirm, t); }
  Action& ref(const char* actionId) { return putStr(Opt::ActionRef, actionId); }
  Action& withFields(const char* spec) { return putStr(Opt::WithFields, spec); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Action)[_key] = String(_label ? _label : _key); }
 private:
  const char* _label;
};

// ── Message (static MUI Alert; always read-only) ───────────────────────────
class Message : public FieldSpec<Message> {
 public:
  Message(const char* key, const char* text) : FieldSpec(key), _text(text) { _af = Access::Read; }
  Message& level(const char* lvl) { return putStr(Opt::Level, lvl); }
  void writeTo(JsonArray& fields) const { open(fields, FieldType::Message)[_key] = String(_text ? _text : ""); }
 private:
  const char* _text;
};

// ── The form ──────────────────────────────────────────────────────────────
// Wraps the old createForm: creates root[name] = { description, fields:[] }.
// add() materializes a field builder into the fields array.
class Form {
 public:
  Form(JsonObject& root, const char* name, const char* description) {
    JsonObject form = root.createNestedObject(name);
    form["description"] = description ? description : "";
    _fields = form.createNestedArray("fields");
  }

  template <typename FieldT>
  Form& add(const FieldT& field) {
    field.writeTo(_fields);
    return *this;
  }

  // Seed runtime rows into a Table field already add()-ed to this form —
  // for REST-only tables whose data is baked into the GET response (e.g. the
  // WiFi scan results, which have no WS channel). Returns the field's value
  // array to fill, or a null array if the key isn't a table on this form.
  JsonArray rowsOf(const char* fieldKey) {
    for (JsonObject f : _fields) {
      if (f.containsKey(fieldKey) && f[fieldKey].is<JsonArray>()) {
        return f[fieldKey].as<JsonArray>();
      }
    }
    return JsonArray();
  }

  // Escape hatch for code still building rows/fields by hand during migration.
  JsonArray& fields() { return _fields; }

 private:
  JsonArray _fields;
};

}  // namespace web

#endif  // WEB_FORM_H
