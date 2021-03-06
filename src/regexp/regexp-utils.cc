// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/regexp/regexp-utils.h"

#include "src/factory.h"
#include "src/isolate.h"
#include "src/objects-inl.h"
#include "src/regexp/jsregexp.h"

namespace v8 {
namespace internal {

// Constants for accessing RegExpLastMatchInfo.
// TODO(jgruber): Currently, RegExpLastMatchInfo is still a JSObject maintained
// and accessed from JS. This is a crutch until all RegExp logic is ported, then
// we can take care of RegExpLastMatchInfo.

Handle<Object> RegExpUtils::GetLastMatchField(Isolate* isolate,
                                              Handle<JSObject> match_info,
                                              int index) {
  return JSReceiver::GetElement(isolate, match_info, index).ToHandleChecked();
}

void RegExpUtils::SetLastMatchField(Isolate* isolate,
                                    Handle<JSObject> match_info, int index,
                                    Handle<Object> value) {
  JSReceiver::SetElement(isolate, match_info, index, value, SLOPPY)
      .ToHandleChecked();
}

int RegExpUtils::GetLastMatchNumberOfCaptures(Isolate* isolate,
                                              Handle<JSObject> match_info) {
  Handle<Object> obj =
      GetLastMatchField(isolate, match_info, RegExpImpl::kLastCaptureCount);
  return Handle<Smi>::cast(obj)->value();
}

Handle<String> RegExpUtils::GetLastMatchSubject(Isolate* isolate,
                                                Handle<JSObject> match_info) {
  return Handle<String>::cast(
      GetLastMatchField(isolate, match_info, RegExpImpl::kLastSubject));
}

Handle<Object> RegExpUtils::GetLastMatchInput(Isolate* isolate,
                                              Handle<JSObject> match_info) {
  return GetLastMatchField(isolate, match_info, RegExpImpl::kLastInput);
}

int RegExpUtils::GetLastMatchCapture(Isolate* isolate,
                                     Handle<JSObject> match_info, int i) {
  Handle<Object> obj =
      GetLastMatchField(isolate, match_info, RegExpImpl::kFirstCapture + i);
  return Handle<Smi>::cast(obj)->value();
}

Handle<String> RegExpUtils::GenericCaptureGetter(Isolate* isolate,
                                                 Handle<JSObject> match_info,
                                                 int capture, bool* ok) {
  const int index = capture * 2;
  if (index >= GetLastMatchNumberOfCaptures(isolate, match_info)) {
    if (ok != nullptr) *ok = false;
    return isolate->factory()->empty_string();
  }

  const int match_start = GetLastMatchCapture(isolate, match_info, index);
  const int match_end = GetLastMatchCapture(isolate, match_info, index + 1);
  if (match_start == -1 || match_end == -1) {
    if (ok != nullptr) *ok = false;
    return isolate->factory()->empty_string();
  }

  if (ok != nullptr) *ok = true;
  Handle<String> last_subject = GetLastMatchSubject(isolate, match_info);
  return isolate->factory()->NewSubString(last_subject, match_start, match_end);
}

namespace {

V8_INLINE bool HasInitialRegExpMap(Isolate* isolate, Handle<JSReceiver> recv) {
  return recv->map() == isolate->regexp_function()->initial_map();
}

}  // namespace

MaybeHandle<Object> RegExpUtils::SetLastIndex(Isolate* isolate,
                                              Handle<JSReceiver> recv,
                                              int value) {
  if (HasInitialRegExpMap(isolate, recv)) {
    JSRegExp::cast(*recv)->SetLastIndex(value);
    return recv;
  } else {
    return Object::SetProperty(recv, isolate->factory()->lastIndex_string(),
                               handle(Smi::FromInt(value), isolate), STRICT);
  }
}

MaybeHandle<Object> RegExpUtils::GetLastIndex(Isolate* isolate,
                                              Handle<JSReceiver> recv) {
  if (HasInitialRegExpMap(isolate, recv)) {
    return handle(JSRegExp::cast(*recv)->LastIndex(), isolate);
  } else {
    return Object::GetProperty(recv, isolate->factory()->lastIndex_string());
  }
}

// ES#sec-regexpexec Runtime Semantics: RegExpExec ( R, S )
// Also takes an optional exec method in case our caller
// has already fetched exec.
MaybeHandle<Object> RegExpUtils::RegExpExec(Isolate* isolate,
                                            Handle<JSReceiver> regexp,
                                            Handle<String> string,
                                            Handle<Object> exec) {
  if (exec->IsUndefined(isolate)) {
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, exec,
        Object::GetProperty(
            regexp, isolate->factory()->NewStringFromAsciiChecked("exec")),
        Object);
  }

  if (exec->IsCallable()) {
    const int argc = 1;
    ScopedVector<Handle<Object>> argv(argc);
    argv[0] = string;

    Handle<Object> result;
    ASSIGN_RETURN_ON_EXCEPTION(
        isolate, result,
        Execution::Call(isolate, exec, regexp, argc, argv.start()), Object);

    if (!result->IsJSReceiver() && !result->IsNull(isolate)) {
      THROW_NEW_ERROR(isolate,
                      NewTypeError(MessageTemplate::kInvalidRegExpExecResult),
                      Object);
    }
    return result;
  }

  if (!regexp->IsJSRegExp()) {
    THROW_NEW_ERROR(isolate,
                    NewTypeError(MessageTemplate::kIncompatibleMethodReceiver,
                                 isolate->factory()->NewStringFromAsciiChecked(
                                     "RegExp.prototype.exec"),
                                 regexp),
                    Object);
  }

  {
    Handle<JSFunction> regexp_exec = isolate->regexp_exec_function();

    const int argc = 1;
    ScopedVector<Handle<Object>> argv(argc);
    argv[0] = string;

    return Execution::Call(isolate, regexp_exec, regexp, argc, argv.start());
  }
}

Maybe<bool> RegExpUtils::IsRegExp(Isolate* isolate, Handle<Object> object) {
  if (!object->IsJSReceiver()) return Just(false);

  Handle<JSReceiver> receiver = Handle<JSReceiver>::cast(object);

  if (isolate->regexp_function()->initial_map() == receiver->map()) {
    // Fast-path for unmodified JSRegExp instances.
    // TODO(ishell): Adapt for new fast-path logic.
    return Just(true);
  }

  Handle<Object> match;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, match,
      JSObject::GetProperty(receiver, isolate->factory()->match_symbol()),
      Nothing<bool>());

  if (!match->IsUndefined(isolate)) return Just(match->BooleanValue());
  return Just(object->IsJSRegExp());
}

bool RegExpUtils::IsBuiltinExec(Handle<Object> exec) {
  if (!exec->IsJSFunction()) return false;

  Code* code = Handle<JSFunction>::cast(exec)->code();
  if (code == nullptr) return false;

  return (code->builtin_index() == Builtins::kRegExpPrototypeExec);
}

// ES#sec-advancestringindex
// AdvanceStringIndex ( S, index, unicode )
int RegExpUtils::AdvanceStringIndex(Isolate* isolate, Handle<String> string,
                                    int index, bool unicode) {
  int increment = 1;

  if (unicode && index < string->length()) {
    const uint16_t first = string->Get(index);
    if (first >= 0xD800 && first <= 0xDBFF && string->length() > index + 1) {
      const uint16_t second = string->Get(index + 1);
      if (second >= 0xDC00 && second <= 0xDFFF) {
        increment = 2;
      }
    }
  }

  return increment;
}

MaybeHandle<Object> RegExpUtils::SetAdvancedStringIndex(
    Isolate* isolate, Handle<JSReceiver> regexp, Handle<String> string,
    bool unicode) {
  Handle<Object> last_index_obj;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, last_index_obj,
      Object::GetProperty(regexp, isolate->factory()->lastIndex_string()),
      Object);

  ASSIGN_RETURN_ON_EXCEPTION(isolate, last_index_obj,
                             Object::ToLength(isolate, last_index_obj), Object);

  const int last_index = Handle<Smi>::cast(last_index_obj)->value();
  const int new_last_index =
      last_index + AdvanceStringIndex(isolate, string, last_index, unicode);

  return SetLastIndex(isolate, regexp, new_last_index);
}

}  // namespace internal
}  // namespace v8
