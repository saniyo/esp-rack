#ifndef web_FormStream_h
#define web_FormStream_h

// FormStream — emit a FormBuilder form to the client INCREMENTALLY over a
// chunked AsyncResponseStream, holding only ONE field / one table row in
// RAM at a time.
//
// Why: ArduinoJson must hold the WHOLE tree contiguously to serialize it.
// A big form (e.g. the endpoints registry: ~40 rows, ~9 KB of pool) can't
// get that block on a no-PSRAM C3 under concurrency, so sendAdaptiveJson
// (which still builds one tree) sheds a 503. FormStream never builds the
// whole tree: it frames the JSON itself and serializes each field/row from
// a TINY scratch doc straight into the response stream, which flushes in
// ~1.5 KB TCP chunks. Peak RAM ≈ one row, so the form ALWAYS streams out
// instead of 503ing on size.
//
// Output is byte-identical to FormBuilder's: callers build each field/row
// with the SAME FormBuilder::addXxxField / per-row fills they'd use
// normally; this class only frames the `fields[]` / table-`rows[]` arrays
// and manages comma separators. The envelope matches createForm():
//   {"<formKey>":{"description":"<title>","fields":[ <field>,<field>,... ]}}
// and a streamed table field is:
//   {"<tableKey>":[ <row>,<row>,... ],"o":"table;..."}
//
// Usage:
//   FormStream fs(request);
//   fs.begin("endpoints", "Endpoints");
//   fs.field([&](JsonArray& a){ FormBuilder::addTextField(a,"total",AF::R,...); });
//   fs.beginTable([&](JsonArray& a){ FormBuilder::addTableField(a,"entries",AF::R,cols...); });
//   for (...) fs.row([&](JsonObject& r){ r["id"]=...; });
//   fs.endTable();
//   fs.end();

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <functional>

class FormStream {
 public:
  explicit FormStream(AsyncWebServerRequest* request)
      : _request(request),
        _stream(request->beginResponseStream("application/json")) {}

  // {"<formKey>":{"description":"<title>","fields":[
  void begin(const char* formKey, const char* title) {
    _stream->print('{');
    printJsonString(formKey);
    _stream->print(F(":{\"description\":"));
    printJsonString(title);
    _stream->print(F(",\"fields\":["));
    _firstField = true;
  }

  // Append one field. `build` receives a fresh JsonArray and must append
  // exactly one field object (any FormBuilder::addXxxField call). The
  // scratch is tiny and DEGRADE-SAFE: if the heap can't even give 512 B
  // the field is skipped, never a crash — so there is no pressure level
  // at which streaming must 503.
  void field(const std::function<void(JsonArray&)>& build) {
    DynamicJsonDocument d(512);
    if (d.capacity() == 0) return;       // out of heap — drop this field
    JsonArray a = d.to<JsonArray>();
    build(a);
    if (a.size() == 0) return;
    sep(_firstField);
    serializeJson(a[0], *_stream);
  }

  // Begin a streamed table field. `build` must append exactly one table
  // field via FormBuilder::addTableField (cols/options, empty rows). We
  // serialize that skeleton, split off its empty `[]` rows array, emit the
  // `{"<key>":[` prefix now and stash the `],"o":"..."}` suffix for
  // endTable(). Degrade-safe: if the skeleton can't build, emit a minimal
  // valid empty table so the overall JSON stays well-formed.
  void beginTable(const std::function<void(JsonArray&)>& build) {
    String prefix;
    _tableSuffix = "";
    DynamicJsonDocument d(768);
    if (d.capacity() > 0) {
      JsonArray a = d.to<JsonArray>();
      build(a);
      if (a.size() > 0) {
        String skel;
        serializeJson(a[0], skel);  // {"<key>":[],"o":"table;..."}
        const int br = skel.indexOf("[]");
        if (br >= 0) {
          prefix       = skel.substring(0, br + 1);  // {"<key>":[
          _tableSuffix = skel.substring(br + 1);     // ],"o":"..."}
        }
      }
    }
    if (prefix.length() == 0) {        // alloc/parse failed — stay valid
      prefix       = F("{\"rows\":[");
      _tableSuffix = F("]}");
    }
    sep(_firstField);            // the table is itself a field in fields[]
    _stream->print(prefix);
    _firstRow = true;
  }

  // One table row. `build` fills a fresh JsonObject (the row). 512 B covers
  // a wide row (7 string columns ≈ 270 B pool), is freed before the next,
  // and is DEGRADE-SAFE: under extreme pressure a row is dropped rather
  // than crashing — peak stays ~one row regardless of count.
  void row(const std::function<void(JsonObject&)>& build) {
    DynamicJsonDocument d(512);
    if (d.capacity() == 0) return;       // out of heap — drop this row
    JsonObject o = d.to<JsonObject>();
    build(o);
    sep(_firstRow);
    serializeJson(o, *_stream);
  }

  void endTable() { _stream->print(_tableSuffix); }  // ],"o":"..."}

  // ]}}  + queue the response.
  void end() {
    _stream->print(F("]}}"));
    _request->send(_stream);
  }

 private:
  AsyncWebServerRequest* _request;
  AsyncResponseStream*   _stream;
  bool   _firstField{true};
  bool   _firstRow{true};
  String _tableSuffix;

  void sep(bool& first) {
    if (!first) _stream->print(',');
    first = false;
  }
  // Escape via ArduinoJson so quotes / control chars in keys or titles are
  // JSON-safe (keys/titles are code literals today, but this keeps the
  // framing honest if a dynamic value ever flows through).
  void printJsonString(const char* s) {
    DynamicJsonDocument d(256);
    d.set(s ? s : "");
    serializeJson(d, *_stream);
  }
};

#endif  // web_FormStream_h
