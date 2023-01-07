/*
 * This file is part of LSPosed.
 *
 * LSPosed is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LSPosed is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LSPosed.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2023 LSPosed Contributors
 */

#include "dex_parser.h"
#include "native_util.h"
#include "slicer/reader.h"

#include <list>
#include <absl/container/flat_hash_map.h>

namespace {
    using AnnotationList = std::vector<std::tuple<jint/*vis*/, jint /*type*/, std::vector<std::tuple<jint /*name*/, jint/*value_type*/, std::string_view/*value*/>>>>;

    void ParseAnnotation(JNIEnv *env, dex::Reader &dex, AnnotationList &annotation_list,
                         std::vector<jint> indices,
                         const dex::AnnotationSetItem *annotation) {
        if (annotation == nullptr) {
            return;
        }
        for (size_t i = 0; i < annotation->size; ++i) {
            auto *item = dex.dataPtr<dex::AnnotationItem>(annotation->entries[i]);
            auto *annotation_data = item->annotation;
            indices.emplace_back(annotation_list.size());
            auto &[visibility, type, items] = annotation_list.emplace_back(item->visibility,
                                                                           dex::ReadULeb128(
                                                                                   &annotation_data),
                                                                           std::vector<std::tuple<jint, jint, std::string_view>>());
            auto size = dex::ReadULeb128(&annotation_data);
            items.resize(size);
            for (size_t j = 0; j < size; ++j) {
                auto &[name, value_type, value] = items[j];
                name = static_cast<jint>(dex::ReadULeb128(&annotation_data));
                auto arg_and_type = *annotation_data++;
                value_type = arg_and_type & 0x1f;
                auto value_arg = arg_and_type >> 5;
                switch (value_type) {
                    case 0x00: // byte
                    case 0x1f: // boolean
                        value = {reinterpret_cast<char *>(const_cast<dex::u1 *>(annotation_data)),
                                 1};
                        break;
                    case 0x02: // short
                    case 0x03: // char
                    case 0x04: // int
                    case 0x06: // long
                    case 0x10: // float
                    case 0x11: // double
                    case 0x17: // string
                    case 0x18: // type
                    case 0x19: // field
                    case 0x1a: // method
                    case 0x1b: // enum
                        value = {
                                reinterpret_cast<char *>(const_cast<dex::u1 *>(annotation_data)),
                                static_cast<size_t>(value_arg)};
                        break;
                    case 0x1c: // array
                    case 0x1d: // annotation
                    case 0x1e: // null
                    default:
                        // not supported
                        break;
                }
            }
        }
    }

    class DexParser : public dex::Reader {
    public:
        DexParser(const dex::u1 *data, size_t size) : dex::Reader(data, size, nullptr, 0) {}

        struct ClassData {
            std::vector<jint> interfaces;
            std::vector<jint> static_fields;
            std::vector<jint> static_fields_access_flags;
            std::vector<jint> instance_fields;
            std::vector<jint> instance_fields_access_flags;
            std::vector<jint> direct_methods;
            std::vector<jint> direct_methods_access_flags;
            std::vector<const dex::Code *> direct_methods_code;
            std::vector<jint> virtual_methods;
            std::vector<jint> virtual_methods_access_flags;
            std::vector<const dex::Code *> virtual_methods_code;
            std::vector<jint> annotations;
        };

        struct MethodBody {
            bool loaded;
            std::vector<jint> referred_strings;
            std::vector<jint> accessed_fields;
            std::vector<jint> assigned_fields;
            std::vector<jint> invoked_methods;
            std::vector<jbyte> opcodes;
        };

        std::vector<ClassData> class_data;
        absl::flat_hash_map<jint, std::vector<jint>> field_annotations;
        absl::flat_hash_map<jint, std::vector<jint>> method_annotations;
        absl::flat_hash_map<jint, std::vector<jint>> parameter_annotations;

        absl::flat_hash_map<jint, MethodBody> method_bodies;
    };
}

namespace lspd {
    LSP_DEF_NATIVE_METHOD(jobject, DexParserBridge, openDex, jobject data, jlongArray args) {
        auto dex_size = env->GetDirectBufferCapacity(data);
        if (dex_size == -1) {
            env->ThrowNew(env->FindClass("java/io/IOException"), "Invalid dex data");
            return nullptr;
        }
        auto *dex_data = env->GetDirectBufferAddress(data);

        auto *dex_reader = new DexParser(reinterpret_cast<dex::u1 *>(dex_data), dex_size);
        env->SetLongArrayRegion(args, 0, 1, reinterpret_cast<const jlong *>(&dex_reader));
        auto &dex = *dex_reader;
        if (dex.IsCompact()) {
            env->ThrowNew(env->FindClass("java/io/IOException"), "Compact dex is not supported");
            return nullptr;
        }
        auto object_class = env->FindClass("java/lang/Object");
        auto string_class = env->FindClass("java/lang/String");
        auto int_array_class = env->FindClass("[I");
        auto out = env->NewObjectArray(7, object_class, nullptr);
        auto out0 = env->NewObjectArray(static_cast<jint>(dex.StringIds().size()),
                                        string_class, nullptr);
        auto strings = dex.StringIds();
        for (size_t i = 0; i < strings.size(); ++i) {
            const auto *ptr = dex.dataPtr<dex::u1>(strings[i].string_data_off);
            [[maybe_unused]] size_t len = dex::ReadULeb128(&ptr);
            auto str = env->NewStringUTF(reinterpret_cast<const char *>(ptr));
            env->SetObjectArrayElement(out0, static_cast<jint>(i), str);
            env->DeleteLocalRef(str);
        }
        env->SetObjectArrayElement(out, 0, out0);
        env->DeleteLocalRef(out0);

        auto types = dex.TypeIds();
        auto out1 = env->NewIntArray(static_cast<jint>(types.size()));
        auto *out1_ptr = env->GetIntArrayElements(out1, nullptr);
        for (size_t i = 0; i < types.size(); ++i) {
            out1_ptr[i] = static_cast<jint>(types[i].descriptor_idx);
        }
        env->ReleaseIntArrayElements(out1, out1_ptr, 0);
        env->SetObjectArrayElement(out, 1, out1);
        env->DeleteLocalRef(out1);

        auto protos = dex.ProtoIds();
        auto out2 = env->NewObjectArray(static_cast<jint>(protos.size()),
                                        int_array_class, nullptr);
        auto empty_type_list = dex::TypeList{.size = 0, .list = {}};
        for (size_t i = 0; i < protos.size(); ++i) {
            auto &proto = protos[i];
            const auto &params = proto.parameters_off ? *dex.dataPtr<dex::TypeList>(
                    proto.parameters_off) : empty_type_list;

            auto out2i = env->NewIntArray(static_cast<jint>(2 + params.size));
            auto *out2i_ptr = env->GetIntArrayElements(out2i, nullptr);
            out2i_ptr[0] = static_cast<jint>(proto.shorty_idx);
            out2i_ptr[1] = static_cast<jint>(proto.return_type_idx);
            for (size_t j = 0; j < params.size; ++j) {
                out2i_ptr[2 + j] = static_cast<jint>(params.list[j].type_idx);
            }
            env->ReleaseIntArrayElements(out2i, out2i_ptr, 0);
            env->SetObjectArrayElement(out2, static_cast<jint>(i), out2i);
            env->DeleteLocalRef(out2i);
        }
        env->SetObjectArrayElement(out, 2, out2);
        env->DeleteLocalRef(out2);

        auto fields = dex.FieldIds();
        auto out3 = env->NewIntArray(static_cast<jint>(3 * fields.size()));
        auto *out3_ptr = env->GetIntArrayElements(out3, nullptr);
        for (size_t i = 0; i < fields.size(); ++i) {
            auto &field = fields[i];
            out3_ptr[3 * i] = static_cast<jint>(field.class_idx);
            out3_ptr[3 * i + 1] = static_cast<jint>(field.type_idx);
            out3_ptr[3 * i + 2] = static_cast<jint>(field.name_idx);
        }
        env->ReleaseIntArrayElements(out3, out3_ptr, 0);
        env->SetObjectArrayElement(out, 3, out3);
        env->DeleteLocalRef(out3);

        auto methods = dex.MethodIds();
        auto out4 = env->NewIntArray(static_cast<jint>(3 * methods.size()));
        auto *out4_ptr = env->GetIntArrayElements(out4, nullptr);
        for (size_t i = 0; i < methods.size(); ++i) {
            out4_ptr[3 * i] = static_cast<jint>(methods[i].class_idx);
            out4_ptr[3 * i + 1] = static_cast<jint>(methods[i].proto_idx);
            out4_ptr[3 * i + 2] = static_cast<jint>(methods[i].name_idx);
        }
        env->ReleaseIntArrayElements(out4, out4_ptr, 0);
        env->SetObjectArrayElement(out, 4, out4);
        env->DeleteLocalRef(out4);

        auto classes = dex.ClassDefs();
        dex.class_data.resize(classes.size());

        AnnotationList annotation_list;

        for (size_t i = 0; i < classes.size(); ++i) {
            auto &class_def = classes[i];

            dex::u4 static_fields_count = 0;
            dex::u4 instance_fields_count = 0;
            dex::u4 direct_methods_count = 0;
            dex::u4 virtual_methods_count = 0;
            const dex::u1 *class_data_ptr = nullptr;

            const dex::AnnotationsDirectoryItem *annotations = nullptr;
            const dex::AnnotationSetItem *class_annotation = nullptr;
            dex::u4 field_annotations_count = 0;
            dex::u4 method_annotations_count = 0;
            dex::u4 parameter_annotations_count = 0;

            auto &class_data = dex.class_data[i];

            if (class_def.interfaces_off) {
                auto defined_interfaces = dex.dataPtr<dex::TypeList>(class_def.interfaces_off);
                class_data.interfaces.resize(defined_interfaces->size);
                for (size_t k = 0; k < class_data.interfaces.size(); ++k) {
                    class_data.interfaces[k] = defined_interfaces->list[k].type_idx;
                }
            }

            if (class_def.annotations_off != 0) {
                annotations = dex.dataPtr<dex::AnnotationsDirectoryItem>(class_def.annotations_off);
                if (annotations->class_annotations_off != 0) {
                    class_annotation = dex.dataPtr<dex::AnnotationSetItem>(
                            annotations->class_annotations_off);
                }
                field_annotations_count = annotations->fields_size;
                method_annotations_count = annotations->methods_size;
                parameter_annotations_count = annotations->parameters_size;
            }

            if (class_def.class_data_off != 0) {
                class_data_ptr = dex.dataPtr<dex::u1>(class_def.class_data_off);
                static_fields_count = dex::ReadULeb128(&class_data_ptr);
                instance_fields_count = dex::ReadULeb128(&class_data_ptr);
                direct_methods_count = dex::ReadULeb128(&class_data_ptr);
                virtual_methods_count = dex::ReadULeb128(&class_data_ptr);
                class_data.static_fields.resize(static_fields_count);
                class_data.static_fields_access_flags.resize(static_fields_count);
                class_data.instance_fields.resize(instance_fields_count);
                class_data.instance_fields_access_flags.resize(instance_fields_count);
                class_data.direct_methods.resize(direct_methods_count);
                class_data.direct_methods_access_flags.resize(direct_methods_count);
                class_data.direct_methods_code.resize(direct_methods_count);
                class_data.virtual_methods.resize(virtual_methods_count);
                class_data.virtual_methods_access_flags.resize(virtual_methods_count);
                class_data.virtual_methods_code.resize(virtual_methods_count);
            }

            if (class_data_ptr) {
                for (size_t k = 0, field_idx = 0; k < static_fields_count; ++k) {
                    class_data.static_fields[k] = static_cast<jint>(field_idx += dex::ReadULeb128(
                            &class_data_ptr));
                    class_data.static_fields_access_flags[k] = static_cast<jint>(dex::ReadULeb128(
                            &class_data_ptr));
                }

                for (size_t k = 0, field_idx = 0; k < instance_fields_count; ++k) {
                    class_data.instance_fields[k] = static_cast<jint>(field_idx += dex::ReadULeb128(
                            &class_data_ptr));
                    class_data.instance_fields_access_flags[k] = static_cast<jint>(dex::ReadULeb128(
                            &class_data_ptr));
                }

                for (size_t k = 0, method_idx = 0; k < direct_methods_count; ++k) {
                    class_data.direct_methods[k] = static_cast<jint>(method_idx += dex::ReadULeb128(
                            &class_data_ptr));
                    class_data.direct_methods_access_flags[k] = static_cast<jint>(dex::ReadULeb128(
                            &class_data_ptr));
                    auto code_off = dex::ReadULeb128(&class_data_ptr);
                    class_data.direct_methods_code[k] = code_off ? dex.dataPtr<dex::Code>(code_off)
                                                                 : nullptr;
                }

                for (size_t k = 0, method_idx = 0; k < virtual_methods_count; ++k) {
                    class_data.virtual_methods[k] = static_cast<jint>(method_idx += dex::ReadULeb128(
                            &class_data_ptr));
                    class_data.virtual_methods_access_flags[k] = static_cast<jint>(dex::ReadULeb128(
                            &class_data_ptr));
                    auto code_off = dex::ReadULeb128(&class_data_ptr);
                    class_data.virtual_methods_code[k] = code_off ? dex.dataPtr<dex::Code>(code_off)
                                                                  : nullptr;
                }
            }

            ParseAnnotation(env, dex, annotation_list, class_data.annotations, class_annotation);

            auto *field_annotations = annotations
                                      ? reinterpret_cast<const dex::FieldAnnotationsItem *>(
                                              annotations + 1) : nullptr;
            for (size_t k = 0; k < field_annotations_count; ++k) {
                auto *field_annotation = dex.dataPtr<dex::AnnotationSetItem>(
                        field_annotations[k].annotations_off);
                ParseAnnotation(env, dex, annotation_list,
                                dex.field_annotations[static_cast<jint>(field_annotations[k].field_idx)],
                                field_annotation);
            }

            auto *method_annotations = field_annotations
                                       ? reinterpret_cast<const dex::MethodAnnotationsItem *>(
                                               field_annotations + field_annotations_count)
                                       : nullptr;
            for (size_t k = 0; k < method_annotations_count; ++k) {
                auto *method_annotation = dex.dataPtr<dex::AnnotationSetItem>(
                        method_annotations[k].annotations_off);
                ParseAnnotation(env, dex, annotation_list,
                                dex.method_annotations[static_cast<jint>(method_annotations[k].method_idx)],
                                method_annotation);
            }

            auto *parameter_annotations = method_annotations
                                          ? reinterpret_cast<const dex::ParameterAnnotationsItem *>(
                                                  method_annotations + method_annotations_count)
                                          : nullptr;
            for (size_t k = 0; k < parameter_annotations_count; ++k) {
                auto *parameter_annotation = dex.dataPtr<dex::AnnotationSetRefList>(
                        parameter_annotations[k].annotations_off);
                auto &indices = dex.parameter_annotations[static_cast<jint>(parameter_annotations[k].method_idx)];
                for (size_t l = 0; l < parameter_annotation->size; ++l) {
                    if (parameter_annotation->list[l].annotations_off != 0) {
                        auto *parameter_annotation_item = dex.dataPtr<dex::AnnotationSetItem>(
                                parameter_annotation->list[l].annotations_off);
                        ParseAnnotation(env, dex, annotation_list, indices,
                                        parameter_annotation_item);
                    }
                    indices.emplace_back(dex::kNoIndex);
                }
            }
        }
        return out;
        auto out5 = env->NewIntArray(static_cast<jint>(2 * annotation_list.size()));
        auto out6 = env->NewObjectArray(static_cast<jint>(2 * annotation_list.size()), object_class,
                                        nullptr);
        auto out5_ptr = env->GetIntArrayElements(out5, nullptr);
        size_t i = 0;
        for (auto &[visibility, type, items]: annotation_list) {
            auto out6i0 = env->NewIntArray(static_cast<jint>(2 * items.size()));
            auto out6i0_ptr = env->GetIntArrayElements(out6i0, nullptr);
            auto out6i1 = env->NewObjectArray(static_cast<jint>(items.size()), object_class,
                                              nullptr);
            size_t j = 0;
            for (auto&[name, value_type, value]: items) {
                auto java_value = env->NewDirectByteBuffer(const_cast<char *>(value.data()),
                                                           value.size());
                env->SetObjectArrayElement(out6i1, static_cast<jint>(j), java_value);
                out6i0_ptr[2 * j] = name;
                out6i0_ptr[2 * j + 1] = value_type;
                env->DeleteLocalRef(java_value);
                ++j;
            }
            env->ReleaseIntArrayElements(out6i0, out6i0_ptr, 0);
            env->SetObjectArrayElement(out6, static_cast<jint>(2 * i), out6i0);
            env->SetObjectArrayElement(out6, static_cast<jint>(2 * i + 1), out6i1);
            out5_ptr[2 * i] = visibility;
            out5_ptr[2 * i + 1] = type;
            env->DeleteLocalRef(out6i0);
            env->DeleteLocalRef(out6i1);
            ++i;
        }
        env->ReleaseIntArrayElements(out5, out5_ptr, 0);
        env->SetObjectArrayElement(out, 5, out5);
        env->SetObjectArrayElement(out, 6, out6);
        env->DeleteLocalRef(out5);
        env->DeleteLocalRef(out6);

        return out;
    }

    LSP_DEF_NATIVE_METHOD(void, DexParserBridge, closeDex, jlong cookie) {
        if (cookie != 0)
            delete reinterpret_cast<DexParser *>(cookie);
    }

    LSP_DEF_NATIVE_METHOD(void, DexParserBridge, visitClass, jlong cookie, jobject visitor,
                          jclass field_visitor_class,
                          jclass method_visitor_class,
                          jobject class_visit_method,
                          jobject field_visit_method,
                          jobject method_visit_method,
                          jobject method_body_visit_method,
                          jobject stop_method) {
        static constexpr dex::u1 kOpcodeMask = 0xff;
        static constexpr dex::u1 kOpcodeNoOp = 0x00;
        static constexpr dex::u1 kOpcodeConstString = 0x1a;
        static constexpr dex::u1 kOpcodeConstStringJumbo = 0x1b;
        static constexpr dex::u1 kOpcodeIGetStart = 0x52;
        static constexpr dex::u1 kOpcodeIGetEnd = 0x58;
        static constexpr dex::u1 kOpcodeSGetStart = 0x60;
        static constexpr dex::u1 kOpcodeSGetEnd = 0x66;
        static constexpr dex::u1 kOpcodeIPutStart = 0x59;
        static constexpr dex::u1 kOpcodeIPutEnd = 0x5f;
        static constexpr dex::u1 kOpcodeSPutStart = 0x67;
        static constexpr dex::u1 kOpcodeSPutEnd = 0x6d;
        static constexpr dex::u1 kOpcodeInvokeStart = 0x6e;
        static constexpr dex::u1 kOpcodeInvokeEnd = 0x72;
        static constexpr dex::u1 kOpcodeInvokeRangeStart = 0x74;
        static constexpr dex::u1 kOpcodeInvokeRangeEnd = 0x78;
        static constexpr dex::u2 kInstPackedSwitchPlayLoad = 0x0100;
        static constexpr dex::u2 kInstSparseSwitchPlayLoad = 0x0200;
        static constexpr dex::u2 kInstFillArrayDataPlayLoad = 0x0300;

        if (cookie == 0) {
            return;
        }
        auto &dex = *reinterpret_cast<DexParser *>(cookie);
        auto *visit_class = env->FromReflectedMethod(class_visit_method);
        auto *visit_field = env->FromReflectedMethod(field_visit_method);
        auto *visit_method = env->FromReflectedMethod(method_visit_method);
        auto *visit_method_body = env->FromReflectedMethod(method_body_visit_method);
        auto *stop = env->FromReflectedMethod(stop_method);

        auto classes = dex.ClassDefs();


        for (size_t i = 0; i < classes.size(); ++i) {
            auto &class_def = classes[i];
            auto &class_data = dex.class_data[i];
            auto interfaces = env->NewIntArray(
                    static_cast<jint>(class_data.interfaces.size()));
            env->SetIntArrayRegion(interfaces, 0,
                                   static_cast<jint>(class_data.interfaces.size()),
                                   class_data.interfaces.data());
            auto static_fields = env->NewIntArray(
                    static_cast<jint>(class_data.static_fields.size()));
            env->SetIntArrayRegion(static_fields, 0,
                                   static_cast<jint>(class_data.static_fields.size()),
                                   class_data.static_fields.data());
            auto static_fields_access_flags = env->NewIntArray(
                    static_cast<jint>(class_data.static_fields_access_flags.size()));
            env->SetIntArrayRegion(static_fields_access_flags, 0,
                                   static_cast<jint>(
                                           class_data.static_fields_access_flags.size()),
                                   class_data.static_fields_access_flags.data());
            auto instance_fields = env->NewIntArray(
                    static_cast<jint>(class_data.instance_fields.size()));
            env->SetIntArrayRegion(instance_fields, 0,
                                   static_cast<jint>(class_data.instance_fields.size()),
                                   class_data.instance_fields.data());
            auto instance_fields_access_flags = env->NewIntArray(
                    static_cast<jint>(class_data.instance_fields_access_flags.size()));
            env->SetIntArrayRegion(instance_fields_access_flags, 0,
                                   static_cast<jint>(
                                           class_data.instance_fields_access_flags.size()),
                                   class_data.instance_fields_access_flags.data());
            auto direct_methods = env->NewIntArray(
                    static_cast<jint>(class_data.direct_methods.size()));
            env->SetIntArrayRegion(direct_methods, 0,
                                   static_cast<jint>(class_data.direct_methods.size()),
                                   class_data.direct_methods.data());
            auto direct_methods_access_flags = env->NewIntArray(
                    static_cast<jint>(class_data.direct_methods_access_flags.size()));
            env->SetIntArrayRegion(direct_methods_access_flags, 0,
                                   static_cast<jint>(
                                           class_data.direct_methods_access_flags.size()),
                                   class_data.direct_methods_access_flags.data());
            auto virtual_methods = env->NewIntArray(
                    static_cast<jint>(class_data.virtual_methods.size()));
            env->SetIntArrayRegion(virtual_methods, 0,
                                   static_cast<jint>(class_data.virtual_methods.size()),
                                   class_data.virtual_methods.data());
            auto virtual_methods_access_flags = env->NewIntArray(
                    static_cast<jint>(class_data.virtual_methods_access_flags.size()));
            env->SetIntArrayRegion(virtual_methods_access_flags, 0,
                                   static_cast<jint>(
                                           class_data.virtual_methods_access_flags.size()),
                                   class_data.virtual_methods_access_flags.data());
            auto class_annotations = env->NewIntArray(
                    static_cast<jint>(class_data.annotations.size()));
            env->SetIntArrayRegion(class_annotations, 0,
                                   static_cast<jint>(class_data.annotations.size()),
                                   class_data.annotations.data());
            jobject member_visitor = env->CallObjectMethod(visitor, visit_class,
                                                           static_cast<jint>(class_def.class_idx),
                                                           static_cast<jint>(class_def.access_flags),
                                                           static_cast<jint>(class_def.superclass_idx),
                                                           interfaces,
                                                           static_cast<jint>(class_def.source_file_idx),
                                                           static_fields,
                                                           static_fields_access_flags,
                                                           instance_fields,
                                                           instance_fields_access_flags,
                                                           direct_methods,
                                                           direct_methods_access_flags,
                                                           virtual_methods,
                                                           virtual_methods_access_flags,
                                                           class_annotations
            );
            env->DeleteLocalRef(interfaces);
            env->DeleteLocalRef(static_fields);
            env->DeleteLocalRef(static_fields_access_flags);
            env->DeleteLocalRef(instance_fields);
            env->DeleteLocalRef(instance_fields_access_flags);
            env->DeleteLocalRef(direct_methods);
            env->DeleteLocalRef(direct_methods_access_flags);
            env->DeleteLocalRef(virtual_methods);
            env->DeleteLocalRef(virtual_methods_access_flags);
            env->DeleteLocalRef(class_annotations);
            if (member_visitor && env->IsInstanceOf(member_visitor, field_visitor_class)) {
                jboolean stopped = JNI_FALSE;
                for (auto &[fields, fields_access_flags]: {
                        std::make_tuple(class_data.static_fields,
                                        class_data.static_fields_access_flags),
                        std::make_tuple(class_data.instance_fields,
                                        class_data.instance_fields_access_flags)}) {
                    for (size_t j = 0; j < fields.size(); j++) {
                        auto field_idx = fields[j];
                        auto access_flags = fields_access_flags[j];
                        auto &field_annotations = dex.field_annotations[field_idx];
                        auto annotations = env->NewIntArray(
                                static_cast<jint>(field_annotations.size()));
                        env->SetIntArrayRegion(annotations, 0,
                                               static_cast<jint>(field_annotations.size()),
                                               field_annotations.data());
                        stopped = env->CallBooleanMethod(member_visitor, visit_field, field_idx,
                                                         access_flags, annotations);
                        env->DeleteLocalRef(annotations);
                        if (stopped == JNI_TRUE) break;
                    }
                    if (stopped == JNI_TRUE) break;
                }
            }
            if (member_visitor && env->IsInstanceOf(member_visitor, method_visitor_class)) {
                jboolean stopped = JNI_FALSE;
                for (auto &[methods, methods_access_flags, methods_code]: {
                        std::make_tuple(class_data.direct_methods,
                                        class_data.direct_methods_access_flags,
                                        class_data.direct_methods_code),
                        std::make_tuple(class_data.virtual_methods,
                                        class_data.virtual_methods_access_flags,
                                        class_data.virtual_methods_code)}) {
                    for (size_t j = 0; j < methods.size(); j++) {
                        auto method_idx = methods[j];
                        auto access_flags = methods_access_flags[j];
                        auto code = methods_code[j];
                        auto method_annotation = dex.method_annotations[method_idx];
                        auto method_annotations = env->NewIntArray(
                                static_cast<jint>(method_annotation.size()));
                        env->SetIntArrayRegion(method_annotations, 0,
                                               static_cast<jint>(method_annotation.size()),
                                               method_annotation.data());
                        auto parameter_annotation = dex.parameter_annotations[method_idx];
                        auto parameter_annotations = env->NewIntArray(
                                static_cast<jint>(parameter_annotation.size()));
                        env->SetIntArrayRegion(parameter_annotations, 0,
                                               static_cast<jint>(parameter_annotation.size()),
                                               parameter_annotation.data());
                        auto body_visitor = env->CallObjectMethod(member_visitor, visit_method,
                                                                  method_idx,
                                                                  access_flags, code != nullptr,
                                                                  method_annotations,
                                                                  parameter_annotations);
                        env->DeleteLocalRef(method_annotations);
                        env->DeleteLocalRef(parameter_annotations);
                        if (body_visitor && code != nullptr) {
                            auto body = dex.method_bodies[method_idx];
                            if (!body.loaded) {
                                const dex::u2 *inst = code->insns;
                                const dex::u2 *end = inst + code->insns_size;
                                while (inst < end) {
                                    dex::u1 opcode = *inst & kOpcodeMask;
                                    body.opcodes.push_back(static_cast<jbyte>(opcode));
                                    if (opcode == kOpcodeConstString) {
                                        auto str_idx = inst[1];
                                        body.referred_strings.push_back(str_idx);
                                    }
                                    if (opcode == kOpcodeConstStringJumbo) {
                                        auto str_idx = *reinterpret_cast<const dex::u4 *>(&inst[1]);
                                        body.referred_strings.push_back(static_cast<jint>(str_idx));
                                    }
                                    if ((opcode >= kOpcodeIGetStart && opcode <= kOpcodeIGetEnd) ||
                                        (opcode >= kOpcodeSGetStart && opcode <= kOpcodeSGetEnd)) {
                                        auto field_idx = inst[1];
                                        body.accessed_fields.push_back(field_idx);
                                    }
                                    if ((opcode >= kOpcodeIPutStart && opcode <= kOpcodeIPutEnd) ||
                                        (opcode >= kOpcodeSPutStart && opcode <= kOpcodeSPutEnd)) {
                                        auto field_idx = inst[1];
                                        body.assigned_fields.push_back(field_idx);
                                    }
                                    if ((opcode >= kOpcodeInvokeStart &&
                                         opcode <= kOpcodeInvokeEnd) ||
                                        (opcode >= kOpcodeInvokeRangeStart &&
                                         opcode <= kOpcodeInvokeRangeEnd)) {
                                        auto callee = inst[1];
                                        body.invoked_methods.push_back(callee);
                                    }
                                    if (opcode == kOpcodeNoOp) {
                                        if (*inst == kInstPackedSwitchPlayLoad) {
                                            inst += inst[1] * 2 + 3;
                                        } else if (*inst == kInstSparseSwitchPlayLoad) {
                                            inst += inst[1] * 4 + 1;
                                        } else if (*inst == kInstFillArrayDataPlayLoad) {
                                            inst += (*reinterpret_cast<const dex::u4 *>(&inst[2]) *
                                                     inst[1] + 1) /
                                                    2 + 3;
                                        }
                                    }
                                    inst += dex::opcode_len[opcode];
                                }
                                body.loaded = true;
                            }
                            auto referred_strings = env->NewIntArray(
                                    static_cast<jint>(body.referred_strings.size()));
                            env->SetIntArrayRegion(referred_strings, 0,
                                                   static_cast<jint>(body.referred_strings.size()),
                                                   body.referred_strings.data());
                            auto accessed_fields = env->NewIntArray(
                                    static_cast<jint>(body.accessed_fields.size()));
                            env->SetIntArrayRegion(accessed_fields, 0,
                                                   static_cast<jint>(body.accessed_fields.size()),
                                                   body.accessed_fields.data());
                            auto assigned_fields = env->NewIntArray(
                                    static_cast<jint>(body.assigned_fields.size()));
                            env->SetIntArrayRegion(assigned_fields, 0,
                                                   static_cast<jint>(body.assigned_fields.size()),
                                                   body.assigned_fields.data());
                            auto invoked_methods = env->NewIntArray(
                                    static_cast<jint>(body.invoked_methods.size()));
                            env->SetIntArrayRegion(invoked_methods, 0,
                                                   static_cast<jint>(body.invoked_methods.size()),
                                                   body.invoked_methods.data());
                            auto opcodes = env->NewByteArray(
                                    static_cast<jint>(body.opcodes.size()));
                            env->SetByteArrayRegion(opcodes, 0,
                                                    static_cast<jint>(body.opcodes.size()),
                                                    body.opcodes.data());
                            env->CallVoidMethod(body_visitor, visit_method_body,
                                                referred_strings,
                                                invoked_methods,
                                                accessed_fields, assigned_fields, opcodes);
                        }
                        stopped = env->CallBooleanMethod(member_visitor, stop);
                        if (stopped == JNI_TRUE) break;
                    }
                    if (stopped == JNI_TRUE) break;
                }
            }
            if (env->CallBooleanMethod(visitor, stop) == JNI_TRUE) break;
        }
    }

    static JNINativeMethod gMethods[] = {
            LSP_NATIVE_METHOD(DexParserBridge, openDex,
                              "(Ljava/nio/buffer/ByteBuffer;[J)Ljava/lang/Object;"),
            LSP_NATIVE_METHOD(DexParserBridge, closeDex, "(J)V;"),
    };


    void RegisterDexParserBridge(JNIEnv *env) {
        REGISTER_LSP_NATIVE_METHODS(DexParserBridge);
    }
}