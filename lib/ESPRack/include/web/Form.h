#ifndef WEB_FORM_H
#define WEB_FORM_H

#include <stdio.h>
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
//  Layout (SES-LIB-029): fields flow in a grid by themselves. web::Cols(n)
//  puts the following fields n per row (fewer on narrow screens) until the
//  next Cols() or a Break(); .span(k) / .fullRow() on a field override its
//  type's default width:
//      form.add(web::Cols(2)).add(web::Number("a", 1)).add(web::Number("b", 2))
//          .add(web::Break()).add(web::Text("note", "..."));
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
  // Layout (SES-LIB-029): k grid columns, or the whole row. Defaults come
  // from the type (numbers, switches, selects: one column; text, tables,
  // trends, messages, actions: the whole row).
  D& span(int k)                { return k > 1 ? putInt(Opt::Span, k) : self(); }
  D& fullRow(bool on = true)    { return on ? putFlag1(Opt::FullRow) : self(); }
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

  // Materializer for "simple" fields. Writes the VALUE key FIRST, then "o".
  // Order MATTERS: the live-status renderer reads Object.keys(field)[0] as the
  // value key (DynamicStatus.tsx), and the legacy FormBuilder also emitted the
  // value before "o". Emitting "o" first makes Status tabs render the raw
  // option string in place of the value.
  template <typename V>
  void emit(JsonArray& fields, const char* type, const V& value) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = value;              // value FIRST
    f["o"] = head(type) + _opts;  // "o" SECOND
  }
};

// ── Simple text-ish fields ────────────────────────────────────────────────
class Text : public FieldSpec<Text> {
 public:
  Text(const char* key, const char* value) : FieldSpec(key), _v(value) {}
  Text& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Text& unit(const char* u)        { return putStr(Opt::Unit, u); }
  Text& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Text, String(_v ? _v : "")); }
 private:
  const char* _v;
};

class Secret : public FieldSpec<Secret> {
 public:
  Secret(const char* key, const char* value) : FieldSpec(key), _v(value) {}
  Secret& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Secret& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Secret, String(_v ? _v : "")); }
 private:
  const char* _v;
};

class Textarea : public FieldSpec<Textarea> {
 public:
  Textarea(const char* key, const String& value) : FieldSpec(key), _v(value) {}
  Textarea& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Textarea& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Textarea, _v); }
 private:
  String _v;
};

class Ip : public FieldSpec<Ip> {
 public:
  Ip(const char* key, const char* value) : FieldSpec(key), _v(value) {}
  Ip& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Ip& unit(const char* u)        { return putStr(Opt::Unit, u); }
  Ip& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Ip, String(_v ? _v : "")); }
 private:
  const char* _v;
};

class TimeZones : public FieldSpec<TimeZones> {
 public:
  TimeZones(const char* key, const char* label) : FieldSpec(key), _v(label) {}
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::TimeZones, String(_v ? _v : "")); }
 private:
  const char* _v;
};

// ── Booleans ────────────────────────────────────────────────────────────--
class Checkbox : public FieldSpec<Checkbox> {
 public:
  Checkbox(const char* key, bool value) : FieldSpec(key), _v(value) {}
  Checkbox& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Checkbox& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Checkbox, _v); }
 private:
  bool _v;
};

class Switch : public FieldSpec<Switch> {
 public:
  Switch(const char* key, bool value) : FieldSpec(key), _v(value) {}
  Switch& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Switch& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Switch, _v); }
 private:
  bool _v;
};

class Button : public FieldSpec<Button> {
 public:
  Button(const char* key, bool value) : FieldSpec(key), _v(value) {}
  Button& placeholder(const char* p) { return putStr(Opt::Placeholder, p); }
  Button& colorMap(const char* spec) { return putStr(Opt::ColorMap, spec); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Button, _v); }
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
    char num[32];  // at the schema's precision; "%.*f", not String()'s padded " 0"
    snprintf(num, sizeof(num), "%.*f", decimals, _v);
    f[_key] = num;
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
  // x labels in the DEVICE timezone (manifest device.tz); x = epoch seconds
  // (>= 1e11 = ms). Without xFormat the x numbers show raw (e.g. millis()).
  //   epoch (time under 36 h of span, else date) | time | date | datetime | dom | month
  Trend& xFormat(const char* f)   { _xFmt = f; return *this; }
  // x is a category key (xAxis): each push REPLACES the data — no timestamp
  // merge, no maxPoints window. Works for barChart and lineChart.
  Trend& category(bool on = true) { _cat = on; return *this; }
  // Stack these series (bars) on top of each other; call again for another stack.
  Trend& stack(const char* a, const char* b, const char* c = nullptr) {
    if (_stack.length()) _stack += '|';
    _stack += a; _stack += '+'; _stack += b;
    if (c && c[0]) { _stack += '+'; _stack += c; }
    return *this;
  }
  // Horizontal line at y = the sibling state key's live value. No ':' '|' ';'
  // in label.
  Trend& refLine(const char* key_, const char* color, const char* label = nullptr) {
    if (_refl.length()) _refl += '|';
    _refl += key_; _refl += ':'; _refl += (color ? color : ""); _refl += ':'; _refl += (label ? label : "");
    return *this;
  }
  // Vertical line at the x the sibling state key's live value names: on a
  // category axis the category equal to it (hidden when there is none), else
  // the nearest x in the data (hidden outside it). The label on top reads
  // "label value". No ':' '|' ';' in label.
  Trend& vrefLine(const char* key_, const char* color, const char* label = nullptr) {
    if (_vrefl.length()) _vrefl += '|';
    _vrefl += key_; _vrefl += ':'; _vrefl += (color ? color : ""); _vrefl += ':'; _vrefl += (label ? label : "");
    return *this;
  }
  // The y axis. yMin / yMax fix an end. yAuto(pad): the free ends follow the
  // visible data with a margin of pad x its span (rounded to a tidy step)
  // instead of starting at 0, e.g. cell voltages: .yAuto(0.05). Without any
  // of them the axis starts at 0 as before.
  Trend& yMin(double v)            { _yMin = v; _y |= 1; return *this; }
  Trend& yMax(double v)            { _yMax = v; _y |= 2; return *this; }
  Trend& yAuto(double pad = 0.05)  { _yPad = pad; _y |= 4; return *this; }
  // A series: its state key, color, curve type and, optionally, the name the
  // legend and the tooltip show (default: the key). No ',' ';' ':' '=' in name.
  Trend& line(const char* key_, const char* color, const char* type, const char* name = nullptr) {
    return line(key_, false, color, type, name);
  }
  Trend& line(const char* key_, bool hidden, const char* color, const char* type, const char* name = nullptr) {
    if (_lines.length()) _lines += ';';
    _lines += key_; _lines += ':';
    if (hidden) _lines += "hidden=true,";
    _lines += "color="; _lines += color;
    _lines += ",type="; _lines += type;
    if (name && name[0]) { _lines += ",name="; _lines += name; }
    return *this;
  }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f.createNestedObject(_key);  // reserved value slot (compat)
    String o = head(FieldType::Trend);
    o += ';'; o += Opt::Mode; o += '='; o += _mode;
    o += ';'; o += Opt::TrendXAxis; o += '=';
    o += (String(_mode) == "barChart" && !_cat) ? String("keys") : _xAxis;
    if (_lines.length())  { o += ';'; o += Opt::Lines; o += '='; o += _lines; }
    if (_legend)  { o += ';'; o += Opt::Legend;  o += "=true"; }
    if (_tooltip) { o += ';'; o += Opt::Tooltip; o += "=true"; }
    if (_maxPoints > 0) { o += ';'; o += Opt::MaxPoints; o += '='; o += _maxPoints; }
    if (_xFmt && _xFmt[0]) { o += ';'; o += Opt::XFormat; o += '='; o += _xFmt; }
    if (_cat) { o += ';'; o += Opt::Category; }
    if (_stack.length()) { o += ';'; o += Opt::Stack; o += '='; o += _stack; }
    if (_refl.length()) { o += ';'; o += Opt::RefLine; o += '='; o += _refl; }
    if (_vrefl.length()) { o += ';'; o += Opt::VRefLine; o += '='; o += _vrefl; }
    if (_y & 1) num(o, Opt::YMin, _yMin);
    if (_y & 2) num(o, Opt::YMax, _yMax);
    if (_y & 4) num(o, Opt::YPad, _yPad);
    o += tail();
    f["o"] = o;
  }
 private:
  static void num(String& o, const char* opt, double v) {
    char b[24];
    snprintf(b, sizeof b, "%g", v);
    o += ';'; o += opt; o += '='; o += b;
  }
  const char* _mode = "lineChart";
  String _xAxis = "timestamp";
  String _lines;
  const char* _xFmt = nullptr;
  bool _cat = false;
  String _stack, _refl, _vrefl;
  double _yMin = 0, _yMax = 0, _yPad = 0;
  uint8_t _y = 0;  // bits: yMin, yMax, yAuto set
  bool _legend = false, _tooltip = false;
  int _maxPoints = 100;
};

// ── Table (schema only; runtime rows stream via WS) ─────────────────────────
class Table : public FieldSpec<Table> {
 public:
  explicit Table(const char* key) : FieldSpec(key) {}
  // type: "text" | "number" | "boolean" | "date" | "time" | "datetime" |
  // "duration" (see FieldVocab.h: dated types use the device timezone; fmt
  // "month" for date, "tod" for time, "s"/"ms"/"min" for duration).
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
  // Time of day: the value is seconds since local midnight (0..86399), shown
  // and edited as HH:MM with no timezone conversion. Clamp it in the updater.
  DateTime& timeOfDay() { return putStr(Opt::DateTimeMode, "tod"); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::DateTime, _v); }
 private:
  int32_t _v;
};

// ── Progress (read-only bar: value against a static or live max) ─────────--
//   web::Progress("today", s.kwh).max(10).maxKey("plan_max").markerKey("plan")
//       .unit("kWh").format("0.0").thresholds("0:error,60:warning,95:success")
// maxKey / markerKey name sibling state keys (on a live tab: any readState
// key; otherwise a field of the same form). thresholds: % of max -> colour.
class Progress : public FieldSpec<Progress> {
 public:
  Progress(const char* key, double value) : FieldSpec(key), _v(value) { _af = Access::Read; }
  Progress& min(double v)                { _min = v; return *this; }
  Progress& max(double v)                { _max = v; return *this; }
  Progress& maxKey(const char* k)        { return putStr(Opt::MaxKey, k); }
  Progress& markerKey(const char* k)     { return putStr(Opt::MarkerKey, k); }
  Progress& unit(const char* u)          { return putStr(Opt::Unit, u); }
  Progress& format(const char* f)        { return putStr(Opt::Format, f); }
  Progress& thresholds(const char* spec) { return putStr(Opt::Thresholds, spec); }
  void writeTo(JsonArray& fields) const {
    JsonObject f = fields.createNestedObject();
    f[_key] = _v;  // value FIRST (see emit)
    String o = head(FieldType::Progress);
    o += ';'; o += Opt::Min; o += '='; o += _min;
    o += ';'; o += Opt::Max; o += '='; o += _max;
    o += tail();
    f["o"] = o;
  }
 private:
  double _v;
  double _min = 0, _max = 100;
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
  // Append the browser's clock on fire: secParam = epoch seconds, msParam (if
  // set) = 0..999. With .ref("system.setTime") this is "set the clock from
  // this browser": withClientTime("epoch", "ms").
  Action& withClientTime(const char* secParam, const char* msParam = nullptr) {
    String v = secParam ? secParam : "";
    if (msParam && msParam[0]) { v += ','; v += msParam; }
    return putStr(Opt::ClientTime, v);
  }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Action, String(_label ? _label : _key)); }
 private:
  const char* _label;
};

// ── Message (static MUI Alert; always read-only) ───────────────────────────
class Message : public FieldSpec<Message> {
 public:
  Message(const char* key, const char* text) : FieldSpec(key), _text(text) { _af = Access::Read; }
  Message& level(const char* lvl) { return putStr(Opt::Level, lvl); }
  // Live severity from a sibling state key whose value is
  // "info" | "warning" | "error" | "success" (else the static level).
  Message& levelKey(const char* k) { return putStr(Opt::LevelKey, k); }
  void writeTo(JsonArray& fields) const { emit(fields, FieldType::Message, String(_text ? _text : "")); }
 private:
  const char* _text;
};

// ── The form ──────────────────────────────────────────────────────────────
// Wraps the old createForm: creates root[name] = { description, fields:[] }.
// add() materializes a field builder into the fields array.
// ── Layout markers (SES-LIB-029) ─────────────────────────────────────────
// Placed between fields: form.add(web::Cols(2)) puts the following fields two
// per row on a wide screen (fewer on narrow ones) until the next Cols() or a
// Break(), which goes back to the default flow (fields as wide as they need,
// wrapping by themselves). Wire: {"t":"cols","n":2} / {"t":"brk"}.
class Cols {
 public:
  explicit Cols(int n) : _n(n < 1 ? 1 : n) {}
  void writeTo(JsonArray& fields) const {
    JsonObject o = fields.createNestedObject();
    o["t"] = "cols";
    o["n"] = _n;
  }

 private:
  int _n;
};

class Break {
 public:
  void writeTo(JsonArray& fields) const {
    JsonObject o = fields.createNestedObject();
    o["t"] = "brk";
  }
};

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
