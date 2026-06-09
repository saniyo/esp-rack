#ifndef WEB_H
#define WEB_H

// ─────────────────────────────────────────────────────────────────────────
//  Umbrella include for the esp-rack web-authoring DSL. One #include gives a
//  consumer the whole fluent surface — "everything the web builds", one style:
//
//    • web::Form / web::Text / web::Number / web::Switch / web::Dropdown /
//      web::Table / web::Trend / web::Action / …   (form + field builders — Form.h)
//    • web::FieldType:: / web::Opt:: / web::Access:: (the wire vocabulary,
//      single source of truth mirrored from the frontend parser — FieldVocab.h)
//    • WebFeature / WebMenu / WebTab / WebAction + WebComponent:: / WebIcon::
//      (manifest / tabs / actions / menu placement — WebFeatureBuilder.h)
//
//  Authoring reads the same fluent way for the manifest and the forms:
//      auto spec = WebFeature("light")
//          .menu(WebMenu("Light").icon(WebIcon::Lightbulb).expandTabs())
//          .tab(WebTab("status", "Status").live())
//          .build();
//      web::Form form(root, "status", "Status");
//      form.add(web::Number("cpu", v).readOnly().min(0).max(500).unit("MHz"));
//
//  Note: parse-back (FormBuilder::updateValue) is a SEPARATE concern from
//  building and still lives in FormBuilder.h — include it explicitly where you
//  parse incoming POST bodies back into state.
// ─────────────────────────────────────────────────────────────────────────

#include "FieldVocab.h"
#include "Form.h"
#include <WebFeatureBuilder.h>

#endif  // WEB_H
