/* AOSP workspace discovery and master catalog tests. */
#include "test_framework.h"
#include "test_helpers.h"

#include "aosp/aosp.h"
#include "aosp/build_graph.h"
#include "aosp/federated_graph.h"
#include "aosp/protocol_graph.h"
#include "aosp/structural_graph.h"
#include "foundation/compat_fs.h"
#include "mcp/mcp.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make_dir(const char *root, const char *relative) {
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/%s", root, relative);
    return th_mkdir_p(path);
}

static int write_relative(const char *root, const char *relative, const char *content) {
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/%s", root, relative);
    return th_write_file(path, content);
}

static int create_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root)
        return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo/manifests") != 0 ||
        make_dir(root, ".repo/local_manifests") != 0 ||
        make_dir(root, "frameworks/base") != 0 ||
        make_dir(root, "frameworks/base/media") != 0 ||
        make_dir(root, "vendor/acme/widgets") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    if (write_relative(root, ".repo/manifest.xml",
            "<?xml version=\"1.0\"?><manifest>"
            "<project name=\"platform/frameworks/base\" path=\"frameworks/base\"/>"
            "<include name=\"extra.xml\"/>"
            "</manifest>") != 0 ||
        write_relative(root, ".repo/manifests/extra.xml",
            "<manifest><project name=\"platform/system/core\" path=\"system/core\"/></manifest>") != 0 ||
        write_relative(root, ".repo/local_manifests/local.xml",
            "<manifest>"
            "<remove-project name=\"platform/system/core\"/>"
            "<project name=\"acme/widgets\" path=\"vendor/acme/widgets\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "frameworks/base/Android.bp",
            "framework_module = \"libframework_audio\"\n"
            "framework_shared = [\"libvendor_audio\"]\n"
            "framework_shared = framework_shared + [\"libmissing\"]\n"
            "framework_shared += [\"libandroid_extra\"]\n"
            "cc_library_shared {\n"
            "  name: framework_module,\n"
            "  shared_libs: framework_shared,\n"
            "  static_libs: [\"libbase_defaults\"],\n"
            "}\n"
            "cc_defaults { name: \"libbase_defaults\" }\n"
            "cc_library { name: \"libandroid_extra\" }\n"
            "aidl_interface { name: \"android.media.audio\", imports: [\"vendor.acme.audio\"] }\n") != 0 ||
        write_relative(root, "frameworks/base/media/IAudioService.aidl",
            "package android.media;\ninterface IAudioService { void start(); }\n") != 0 ||
        write_relative(root, "frameworks/base/media/jni.cpp",
            "static void nativeClose() {}\n"
            "static const JNINativeMethod gMethods[] = {\n"
            "  {\"nativeClose\", \"()V\", (void*) nativeClose},\n"
            "};\n"
            "registerNativeMethods(env, \"android/media/AudioSystem\", gMethods, 1);\n") != 0 ||
        write_relative(root, "vendor/acme/widgets/Android.bp",
            "aidl_interface { name: \"vendor.acme.audio\" }\n") != 0 ||
        write_relative(root, "vendor/acme/widgets/Android.mk",
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := libvendor_audio\n"
            "LOCAL_SHARED_LIBRARIES := libbase_defaults\n"
            "include $(BUILD_SHARED_LIBRARY)\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_q7_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_q7_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "alpha/src") != 0 ||
        make_dir(root, "beta/src") != 0 || make_dir(root, "gamma/src") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"platform/q7-alpha\" path=\"alpha\"/>"
            "<project name=\"platform/q7-beta\" path=\"beta\"/>"
            "<project name=\"platform/q7-gamma\" path=\"gamma\"/>"
            "<project name=\"vendor/q7-missing\" path=\"vendor/missing\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "alpha/src/Alpha.c",
            "int Q7Start(void) {\n"
            "  return Q7Local();\n"
            "}\n\n"
            "int Q7Local(void) {\n"
            "  return 1;\n"
            "}\n\n"
            "int Duplicate(void) {\n"
            "  return 10;\n"
            "}\n") != 0 ||
        write_relative(root, "beta/src/Beta.c",
            "int Q7Bridge(void) {\n"
            "  return 2;\n"
            "}\n\n"
            "int Duplicate(void) {\n"
            "  return 20;\n"
            "}\n\n"
            "int Q7BetaLocal(void) {\n"
            "  return 21;\n"
            "}\n") != 0 ||
        write_relative(root, "gamma/src/Gamma.c",
            "int Q7End(void) {\n"
            "  return 3;\n"
            "}\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_defaults_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_defaults_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "project") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest><project name=\"platform/defaults\" path=\"project\"/></manifest>") != 0 ||
        write_relative(root, "project/Android.bp",
            "cc_defaults {\n"
            "  name: \"base_defaults\",\n"
            "  shared_libs: [\"libbase\", \"libdirect\"],\n"
            "  static_libs: [\"libstatic\"],\n"
            "}\n"
            "cc_defaults {\n"
            "  name: \"mid_defaults\",\n"
            "  defaults: [\"base_defaults\"],\n"
            "  shared_libs: [\"libmid\"],\n"
            "}\n"
            "cc_library {\n"
            "  name: \"libconsumer\",\n"
            "  defaults: [\"mid_defaults\"],\n"
            "  shared_libs: [\"libdirect\"],\n"
            "}\n"
            "cc_defaults { name: \"cycle_a\", defaults: [\"cycle_b\"], shared_libs: [\"libcycle_a\"] }\n"
            "cc_defaults { name: \"cycle_b\", defaults: [\"cycle_a\"], shared_libs: [\"libcycle_b\"] }\n"
            "cc_library { name: \"libcycle_consumer\", defaults: [\"cycle_a\"] }\n"
            "cc_library { name: \"libbase\" }\n"
            "cc_library { name: \"libstatic\" }\n"
            "cc_library { name: \"libmid\" }\n"
            "cc_library { name: \"libdirect\" }\n"
            "cc_library { name: \"libcycle_a\" }\n"
            "cc_library { name: \"libcycle_b\" }\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_variants_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_variants_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "project") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest><project name=\"platform/variants\" path=\"project\"/></manifest>") != 0 ||
        write_relative(root, "project/Android.bp",
            "cc_defaults {\n"
            "  name: \"variant_defaults\",\n"
            "  arch: { arm64: { shared_libs: [\"libfromdefaults\"] } },\n"
            "}\n"
            "cc_library {\n"
            "  name: \"libvariant_consumer\",\n"
            "  compile_multilib: \"both\",\n"
            "  shared_libs: [\"libcommon\"],\n"
            "  target: {\n"
            "    android: {\n"
            "      defaults: [\"variant_defaults\"],\n"
            "      shared_libs: [\"libtarget\", \"libdup\"],\n"
            "    },\n"
            "    host: { shared_libs: [\"libhost\"] },\n"
            "  },\n"
            "  arch: { arm64: { static_libs: [\"libarch\"], shared_libs: [\"libdup\"] } },\n"
            "  multilib: {\n"
            "    lib32: { shared_libs: [\"lib32\"] },\n"
            "    lib64: { shared_libs: [\"lib64\"] },\n"
            "  },\n"
            "  product_variables: { debuggable: { shared_libs: [\"libdebug\"] } },\n"
            "  soong_config_variables: { feature: {\n"
            "    enabled: { shared_libs: [\"libfeature\"] },\n"
            "    conditions_default: { shared_libs: [\"libdefault\"] },\n"
            "  } },\n"
            "  runtime_libs: select(soong_config_variable(\"acme\", \"mode\"), {\n"
            "    \"enabled\": [\"libselect\"],\n"
            "    default: [],\n"
            "  }),\n"
            "}\n"
            "cc_library { name: \"libcommon\" }\n"
            "cc_library { name: \"libtarget\" }\n"
            "cc_library { name: \"libhost\" }\n"
            "cc_library { name: \"libarch\" }\n"
            "cc_library { name: \"lib32\" }\n"
            "cc_library { name: \"lib64\" }\n"
            "cc_library { name: \"libdebug\" }\n"
            "cc_library { name: \"libfeature\" }\n"
            "cc_library { name: \"libdefault\" }\n"
            "cc_library { name: \"libdup\" }\n"
            "cc_library { name: \"libfromdefaults\" }\n"
            "cc_library { name: \"libselect\" }\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_build_boundaries_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_boundaries_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "global") != 0 ||
        make_dir(root, "vendor/one") != 0 || make_dir(root, "vendor/two") != 0 ||
        make_dir(root, "app/client/sub") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"platform/global\" path=\"global\"/>"
            "<project name=\"vendor/one\" path=\"vendor/one\"/>"
            "<project name=\"vendor/two\" path=\"vendor/two\"/>"
            "<project name=\"platform/client\" path=\"app/client\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "global/Android.bp",
            "cc_library { name: \"libglobal\", visibility: [\"//visibility:public\"] }\n") != 0 ||
        write_relative(root, "vendor/one/Android.bp",
            "soong_namespace {}\n"
            "cc_library { name: \"libsame\", visibility: [\"//visibility:public\"] }\n"
            "cc_library { name: \"libduplicate\", visibility: [\"//visibility:public\"] }\n"
            "cc_library { name: \"libprivate\", visibility: [\"//visibility:private\"] }\n"
            "cc_library { name: \"libpackage\", visibility: [\"//app/client:__pkg__\"] }\n"
            "cc_library { name: \"libsub\", visibility: [\"//app/client:__subpackages__\"] }\n"
            "cc_library { name: \"libunknown\", visibility: [\"//visibility:any_partition\"] }\n") != 0 ||
        write_relative(root, "vendor/two/Android.bp",
            "soong_namespace {}\n"
            "cc_library { name: \"libsame\", visibility: [\"//visibility:public\"] }\n"
            "cc_library { name: \"libduplicate\", visibility: [\"//visibility:public\"] }\n"
            "cc_library { name: \"libqualified\", visibility: [\"//visibility:public\"] }\n") != 0 ||
        write_relative(root, "app/client/Android.bp",
            "soong_namespace { imports: [\"vendor/one\", \"vendor/two\"] }\n"
            "package { default_visibility: [\"//visibility:private\"] }\n"
            "cc_library { name: \"libsame\" }\n"
            "cc_library {\n"
            "  name: \"libconsumer\",\n"
            "  shared_libs: [\n"
            "    \"libglobal\", \"libsame\", \"libduplicate\", \"libprivate\",\n"
            "    \"libpackage\", \"libsub\", \"libunknown\",\n"
            "    \"//vendor/two:libqualified\", \"libmissing\",\n"
            "  ],\n"
            "}\n") != 0 ||
        write_relative(root, "app/client/sub/Android.bp",
            "cc_library { name: \"libsubconsumer\", "
            "shared_libs: [\"libpackage\", \"libsub\"] }\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_generated_build_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_generated_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "project") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest><project name=\"platform/generated\" path=\"project\"/></manifest>") != 0 ||
        write_relative(root, "project/Android.bp",
            "filegroup { name: \"input_files\", srcs: [\"input.txt\"] }\n"
            "filegroup {\n"
            "  name: \"common_srcs\",\n"
            "  path: \"src\",\n"
            "  srcs: [\"a.cpp\", \":generated{.cpp}\"],\n"
            "}\n"
            "genrule {\n"
            "  name: \"generated\",\n"
            "  tools: [\"host_tool\"],\n"
            "  tool_files: [\"script.py\"],\n"
            "  srcs: [\":input_files\", \"schema.json\"],\n"
            "  out: [\"generated.cpp\", \"generated.h\"],\n"
            "  cmd: \"$(location host_tool) $(in) $(out)\",\n"
            "}\n"
            "cc_defaults {\n"
            "  name: \"b5_defaults\",\n"
            "  srcs: [\"inherited.cpp\"],\n"
            "  generated_sources: [\":generated{.cpp}\"],\n"
            "}\n"
            "cc_binary {\n"
            "  name: \"consumer\",\n"
            "  defaults: [\"b5_defaults\"],\n"
            "  srcs: [\":common_srcs\", \":generated{.cpp}\", \"main.cpp\", "
            "\":missing_generator{.src}\"],\n"
            "  generated_sources: [\"generated\"],\n"
            "  generated_headers: [\"generated\"],\n"
            "  export_generated_headers: [\"generated\"],\n"
            "  tools: [\"host_tool\"],\n"
            "}\n"
            "sh_binary { name: \"host_tool\" }\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_make_semantics_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_make_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "project") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest><project name=\"platform/make\" path=\"project\"/></manifest>") != 0 ||
        write_relative(root, "project/Android.bp",
            "cc_library { name: \"libbase\" }\n"
            "cc_library { name: \"libutils\" }\n"
            "cc_library_static { name: \"libcodec_vendor\" }\n") != 0 ||
        write_relative(root, "project/common.mk",
            "feature_enabled := true\n"
            "define vendor-lib\n"
            "lib$(1)_vendor\n"
            "endef\n") != 0 ||
        write_relative(root, "project/Android.mk",
            "LOCAL_PATH := $(call my-dir)\n"
            "default_lib ?= $(default_target)\n"
            "default_lib ?= $(UNSUPPORTED_DEFAULT)\n"
            "default_target := libbase\n"
            "late_libs += $(late_target)\n"
            "late_target := libutils\n"
            "base_libs = $(default_lib) $(late_libs)\n"
            "include $(LOCAL_PATH)/common.mk\n"
            "ifeq ($(feature_enabled),true)\n"
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := libmake_audio\n"
            "LOCAL_MODULE_CLASS := SHARED_LIBRARIES\n"
            "LOCAL_SHARED_LIBRARIES := $(base_libs)\n"
            "LOCAL_STATIC_LIBRARIES += $(call vendor-lib,codec)\n"
            "LOCAL_SRC_FILES := audio.cpp \\\n"
            "  generated.cpp\n"
            "include $(BUILD_SHARED_LIBRARY)\n"
            "else\n"
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := ignored_else\n"
            "include $(BUILD_SHARED_LIBRARY)\n"
            "endif\n"
            "ifneq ($(feature_enabled),true)\n"
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := ignored_ifneq\n"
            "include $(BUILD_EXECUTABLE)\n"
            "endif\n"
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := make_app\n"
            "LOCAL_MODULE_CLASS := APPS\n"
            "LOCAL_REQUIRED_MODULES := libmake_audio\n"
            "LOCAL_PREBUILT_MODULE_FILE := app.apk\n"
            "include $(BUILD_PREBUILT)\n"
            "ifeq ($(TARGET_ARCH),arm64)\n"
            "include $(CLEAR_VARS)\n"
            "LOCAL_MODULE := unknown_arch_module\n"
            "include $(BUILD_EXECUTABLE)\n"
            "endif\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_product_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_product_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "device/acme/demo") != 0 ||
        make_dir(root, "vendor/acme/demo") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"device/acme/demo\" path=\"device/acme/demo\"/>"
            "<project name=\"vendor/acme/demo\" path=\"vendor/acme/demo\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "device/acme/demo/Android.bp",
            "android_app { name: \"DirectApp\" }\n"
            "android_app { name: \"DeviceApp\" }\n") != 0 ||
        write_relative(root, "vendor/acme/demo/Android.bp",
            "android_app { name: \"VendorApp\" }\n"
            "android_app { name: \"BaseApp\" }\n") != 0 ||
        write_relative(root, "device/acme/demo/aosp_demo.mk",
            "$(call inherit-product, device/acme/demo/device.mk)\n"
            "$(call inherit-product-if-exists, vendor/acme/demo/missing.mk)\n"
            "PRODUCT_NAME := aosp_demo\n"
            "PRODUCT_DEVICE := demo\n"
            "PRODUCT_BRAND := Acme\n"
            "PRODUCT_MODEL := Demo Phone\n"
            "PRODUCT_MANUFACTURER := Acme Devices\n"
            "PRODUCT_PACKAGES += DirectApp\n"
            "PRODUCT_PACKAGES_VENDOR += VendorApp MissingVendor\n"
            "PRODUCT_COPY_FILES += device/acme/demo/init.demo.rc:vendor/etc/init/init.demo.rc "
            "device/acme/demo/permissions.xml:product/etc/permissions/demo.xml\n") != 0 ||
        write_relative(root, "device/acme/demo/device.mk",
            "$(call inherit-product, vendor/acme/demo/vendor.mk)\n"
            "PRODUCT_PACKAGES += DeviceApp\n") != 0 ||
        write_relative(root, "vendor/acme/demo/vendor.mk",
            "PRODUCT_PACKAGES += BaseApp\n") != 0 ||
        write_relative(root, "device/acme/demo/cycle_a.mk",
            "$(call inherit-product, device/acme/demo/cycle_b.mk)\n"
            "PRODUCT_PACKAGES += CycleA\n") != 0 ||
        write_relative(root, "device/acme/demo/cycle_b.mk",
            "$(call inherit-product, device/acme/demo/cycle_a.mk)\n"
            "PRODUCT_PACKAGES += CycleB\n") != 0 ||
        write_relative(root, "device/acme/demo/BoardConfig.mk",
            "TARGET_BOARD_PLATFORM := demo\n"
            "BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4\n"
            "BOARD_PRODUCTIMAGE_PARTITION_SIZE := 1048576\n"
            "BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4\n"
            "BOARD_ODMIMAGE_PARTITION_SIZE := $(UNKNOWN_SIZE)\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_bazel_mixed_build_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_bazel_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "app") != 0 ||
        make_dir(root, "vendor/lib") != 0 || make_dir(root, "vendor/dup1") != 0 ||
        make_dir(root, "vendor/dup2") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"platform/app\" path=\"app\"/>"
            "<project name=\"vendor/lib\" path=\"vendor/lib\"/>"
            "<project name=\"vendor/dup1\" path=\"vendor/dup1\"/>"
            "<project name=\"vendor/dup2\" path=\"vendor/dup2\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "app/Android.bp",
            "cc_binary { name: \"app_bin\" }\n"
            "cc_library { name: \"local_dep\" }\n") != 0 ||
        write_relative(root, "vendor/lib/Android.bp",
            "cc_library { name: \"libshared\" }\n"
            "cc_library { name: \"libvariant_arm\" }\n"
            "cc_library { name: \"libvariant_x86\" }\n") != 0 ||
        write_relative(root, "vendor/dup1/Android.bp",
            "cc_library { name: \"libduplicate\" }\n") != 0 ||
        write_relative(root, "vendor/dup2/Android.bp",
            "cc_library { name: \"libduplicate\" }\n") != 0 ||
        write_relative(root, "app/aosp_bazel_mixed_build.json",
            "{"
            "\"schema\":\"aosp_mixed_build_metadata\",\"version\":1,"
            "\"unsupported\":[\"select_provider:FeatureFlagInfo\"],"
            "\"targets\":["
            "{\"label\":\"//app:app\",\"module_name\":\"app_bin\","
            "\"kind\":\"cc_binary\",\"unsupported\":[\"provider:AndroidIdeInfo\"],"
            "\"dependencies\":["
            "{\"label\":\"//vendor/lib:shared\",\"configuration\":\"android_arm64\","
            "\"type\":\"BAZEL_LINK\",\"transition\":\"target\"},"
            "\"//vendor/lib:variant\",\"//missing:target\","
            "\"@rules_cc//cc:toolchain\",\"//app:duplicate\","
            "\"//app:unmapped\",\":local\"]},"
            "{\"label\":\"//app:local\",\"module_name\":\"local_dep\",\"kind\":\"cc_library\"},"
            "{\"label\":\"//app:duplicate\",\"module_name\":\"libduplicate\"},"
            "{\"label\":\"//app:unmapped\",\"module_name\":\"missing_module\"}"
            "]}") != 0 ||
        write_relative(root, "vendor/lib/aosp_bazel_mixed_build.json",
            "{"
            "\"schema\":\"aosp_mixed_build_metadata\",\"version\":1,\"targets\":["
            "{\"label\":\"//vendor/lib:shared\",\"configuration\":\"android_arm64\","
            "\"module_name\":\"libshared\",\"kind\":\"cc_library\"},"
            "{\"label\":\"//vendor/lib:variant\",\"configuration\":\"android_arm64\","
            "\"module_name\":\"libvariant_arm\"},"
            "{\"label\":\"//vendor/lib:variant\",\"configuration\":\"android_x86_64\","
            "\"module_name\":\"libvariant_x86\"}"
            "]}") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_build_file_links_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_file_links_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "app/src") != 0 ||
        make_dir(root, "vendor") != 0 || make_dir(root, "unindexed") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"platform/app\" path=\"app\"/>"
            "<project name=\"vendor/generated\" path=\"vendor\"/>"
            "<project name=\"platform/unindexed\" path=\"unindexed\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "app/Android.bp",
            "filegroup { name: \"linked_files\", path: \"src\", srcs: [\"a.cpp\"] }\n"
            "filegroup { name: \"invalid_path_group\", path: \"$(escaped)\", srcs: [\"a.cpp\"] }\n"
            "genrule { name: \"local_gen\", out: [\"gen.cpp\", \"gen.h\"] }\n"
            "cc_binary {\n"
            "  name: \"linked_consumer\",\n"
            "  srcs: [\"main.cpp\", \"missing.cpp\", \"../../escape.cpp\", "
            "\":local_gen{.cpp}\"],\n"
            "  generated_headers: [\":local_gen{.header}\"],\n"
            "  generated_sources: [\"vendor_gen\"],\n"
            "  tool_files: [\"ambiguous.py\"],\n"
            "}\n") != 0 ||
        write_relative(root, "app/main.cpp", "int linked_main(void) { return 1; }\n") != 0 ||
        write_relative(root, "app/src/a.cpp", "int linked_a(void) { return 2; }\n") != 0 ||
        write_relative(root, "app/ambiguous.py", "def tool(): return 1\n") != 0 ||
        write_relative(root, "vendor/Android.bp",
            "genrule { name: \"vendor_gen\", out: [\"vendor.out\"] }\n") != 0 ||
        write_relative(root, "unindexed/Android.bp",
            "cc_library { name: \"unindexed_lib\", srcs: [\"unindexed.cpp\"] }\n") != 0 ||
        write_relative(root, "unindexed/unindexed.cpp",
            "int unindexed_symbol(void) { return 3; }\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_complete_aidl_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_aidl_p1_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "api/android/test") != 0 ||
        make_dir(root, "callbacks/com/acme") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"platform/api\" path=\"api\"/>"
            "<project name=\"vendor/callbacks\" path=\"callbacks\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "callbacks/com/acme/ICallback.aidl",
            "package com.acme;\n"
            "@VintfStability\n"
            "oneway interface ICallback {\n"
            "  void onEvent(int event);\n"
            "}\n") != 0 ||
        write_relative(root, "api/android/test/Result.aidl",
            "package android.test;\n"
            "@JavaOnlyStableParcelable\n"
            "parcelable Result {\n"
            "  @nullable String message;\n"
            "  int code;\n"
            "  int mode = DEFAULT_MODE;\n"
            "  @FieldTag(value=1) int tagged;\n"
            "}\n") != 0 ||
        write_relative(root, "api/android/test/Payload.aidl",
            "package android.test;\n"
            "union Payload {\n"
            "  int number;\n"
            "  Result result;\n"
            "}\n") != 0 ||
        write_relative(root, "api/android/test/Status.aidl",
            "package android.test;\n"
            "@Backing(type=\"int\")\n"
            "enum Status { UNKNOWN = 0, READY = 1, }\n") != 0 ||
        write_relative(root, "api/android/test/IService.aidl",
            "package android.test;\n"
            "import com.acme.ICallback;\n"
            "import android.test.Result;\n"
            "import android.test.Missing;\n"
            "@VintfStability\n"
            "interface IService {\n"
            "  @EnforcePermission(\"android.permission.TEST\")\n"
            "  oneway void registerCallback(in ICallback callback);\n"
            "  Result fetch(in Status status);\n"
            "}\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_binder_flow_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_binder_p2_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "service/android/media") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest><project name=\"platform/service\" path=\"service\"/></manifest>") != 0 ||
        write_relative(root, "service/android/media/IAudioService.aidl",
            "package android.media;\n"
            "interface IAudioService { int start(); void stop(); }\n") != 0 ||
        write_relative(root, "service/android/media/AudioBinder.cpp",
            "class AudioService : public BnAudioService {\n"
            " public:\n"
            "  int start() override { return 0; }\n"
            "  void stop() override {}\n"
            "};\n"
            "enum { TRANSACTION_start = 1, TRANSACTION_stop = 2 };\n"
            "int BnAudioService::start() { return 0; }\n"
            "void BnAudioService::stop() {}\n"
            "int BnAudioService::onTransact(int code) {\n"
            "  if (code == TRANSACTION_start) return start();\n"
            "  if (code == TRANSACTION_stop) { stop(); return 0; }\n"
            "  return -1;\n"
            "}\n"
            "int BpAudioService::start() {\n"
            "  return remote()->transact(TRANSACTION_start);\n"
            "}\n"
            "void BpAudioService::stop() {\n"
            "  remote()->transact(TRANSACTION_stop);\n"
            "}\n"
            "class Unrelated { public: int start(); };\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_binder_backends_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_binder_p4_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "service/android/media") != 0 ||
        make_dir(root, "service/generated") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest><project name=\"platform/service\" path=\"service\"/></manifest>") != 0 ||
        write_relative(root, "service/android/media/IAudioService.aidl",
            "package android.media;\ninterface IAudioService { void run(); }\n") != 0 ||
        write_relative(root, "service/generated/IAudioService.java",
            "package android.media;\n"
            "public interface IAudioService {\n"
            "  abstract class Stub {\n"
            "    static final int TRANSACTION_run = 1;\n"
            "    public void run() {}\n"
            "    public boolean onTransact(int code) { if (code == TRANSACTION_run) { run(); return true; } return false; }\n"
            "    static class Proxy {\n"
            "      public void run() { remote.transact(TRANSACTION_run); }\n"
            "    }\n"
            "  }\n"
            "}\n") != 0 ||
        write_relative(root, "service/generated/JavaService.java",
            "package android.media;\n"
            "class JavaService extends IAudioService.Stub {\n"
            "  public void run() {}\n"
            "}\n") != 0 ||
        write_relative(root, "service/generated/IAudioService.cpp",
            "class CppService : public BnAudioService {\n"
            " public: void run() {}\n"
            "};\n"
            "enum { TRANSACTION_run = 1 };\n"
            "void BnAudioService::run() {}\n"
            "int BnAudioService::onTransact(int code) { if (code == TRANSACTION_run) { run(); return 0; } return -1; }\n"
            "void BpAudioService::run() { remote()->transact(TRANSACTION_run); }\n") != 0 ||
        write_relative(root, "service/generated/IAudioService.rs",
            "pub struct RustService;\n"
            "impl IAudioService for RustService {\n"
            "  fn run(&self) {}\n"
            "}\n"
            "mod transactions { pub const run: u32 = 1; }\n"
            "pub struct BnAudioService;\n"
            "impl BnAudioService {\n"
            "  pub fn run(&self) {}\n"
            "  pub fn on_transact(&self, code: u32) { if code == transactions::run { self.run(); } }\n"
            "}\n"
            "pub struct BpAudioService;\n"
            "impl BpAudioService {\n"
            "  pub fn run(&self) { let code = transactions::run; self.binder.transact(code); }\n"
            "}\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

static int create_service_manager_workspace_fixture(char **root_out) {
    static unsigned fixture_sequence = 0;
    char prefix[64];
    (void)snprintf(prefix, sizeof(prefix), "cbm_aosp_service_p3_%u", ++fixture_sequence);
    const char *temp_root = th_mktempdir(prefix);
    if (!temp_root) return -1;
    char *root = strdup(temp_root);
    if (!root) return -1;
    if (make_dir(root, ".repo") != 0 || make_dir(root, "server/android/media") != 0 ||
        make_dir(root, "client/one") != 0 || make_dir(root, "client/two") != 0 ||
        write_relative(root, ".repo/manifest.xml",
            "<manifest>"
            "<project name=\"platform/server\" path=\"server\"/>"
            "<project name=\"platform/client\" path=\"client\"/>"
            "</manifest>") != 0 ||
        write_relative(root, "server/android/media/IAudioService.aidl",
            "package android.media;\ninterface IAudioService { int start(); }\n") != 0 ||
        write_relative(root, "client/one/IDuplicate.aidl",
            "package one.media;\ninterface IDuplicate { void ping(); }\n") != 0 ||
        write_relative(root, "client/two/IDuplicate.aidl",
            "package two.media;\ninterface IDuplicate { void ping(); }\n") != 0 ||
        write_relative(root, "server/AudioServer.cpp",
            "class BnAudioService {};\n"
            "class AudioService : public BnAudioService {\n"
            " public:\n"
            "  int start() { return 0; }\n"
            "};\n"
            "void publishAudio() {\n"
            "  defaultServiceManager()->addService(String16(\"media.audio\"), new AudioService());\n"
            "}\n"
            "void publishDynamic(const char* name) {\n"
            "  defaultServiceManager()->addService(String16(name), new AudioService(\"debug.alias\"));\n"
            "}\n"
            "void publishNdk() {\n"
            "  AServiceManager_addService(binder, \"ndk.audio\");\n"
            "}\n"
            "class OtherAudioService : public BnAudioService {\n"
            " public:\n"
            "  int start() { return 1; }\n"
            "};\n"
            "void publishAmbiguousOne() {\n"
            "  defaultServiceManager()->addService(String16(\"ambiguous.server\"), new AudioService());\n"
            "}\n"
            "void publishAmbiguousTwo() {\n"
            "  defaultServiceManager()->addService(String16(\"ambiguous.server\"), new OtherAudioService());\n"
            "}\n") != 0 ||
        write_relative(root, "client/AudioClient.cpp",
            "void connectAudio() {\n"
            "  auto service = interface_cast<IAudioService>(\n"
            "      defaultServiceManager()->getService(String16(\"media.audio\")));\n"
            "}\n"
            "void waitAudio() {\n"
            "  auto service = interface_cast<IAudioService>(\n"
            "      defaultServiceManager()->waitForService(String16(\"media.audio\")));\n"
            "}\n"
            "void checkMissing() {\n"
            "  auto binder = defaultServiceManager()->checkService(String16(\"missing.audio\"));\n"
            "}\n") != 0 ||
        write_relative(root, "client/JavaClient.java",
            "class JavaClient {\n"
            "  void connectJava() {\n"
            "    IAudioService.Stub.asInterface(ServiceManager.getService(\"media.audio\"));\n"
            "  }\n"
            "  void connectAmbiguous() {\n"
            "    IDuplicate.Stub.asInterface(ServiceManager.getService(\"ambiguous.audio\"));\n"
            "  }\n"
            "}\n") != 0) {
        th_rmtree(root);
        free(root);
        return -1;
    }
    *root_out = root;
    return 0;
}

TEST(aosp_manifest_include_and_local_override) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 2);
    ASSERT_STR_EQ(workspace.repos[0].path, "frameworks/base");
    ASSERT_STR_EQ(workspace.repos[1].path, "vendor/acme/widgets");
    ASSERT(workspace.repos[0].exists);
    ASSERT(workspace.repos[1].exists);
    ASSERT_EQ((int)strlen(workspace.workspace_id), CBM_AOSP_ID_LEN);
    ASSERT_EQ((int)strlen(workspace.manifest_hash), CBM_AOSP_HASH_LEN);
    ASSERT(strcmp(workspace.repos[0].repo_id, workspace.repos[1].repo_id) != 0);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_manifest_rejects_parent_path) {
    const char *temp_root = th_mktempdir("cbm_aosp_unsafe");
    ASSERT(temp_root != NULL);
    char *root = strdup(temp_root);
    ASSERT(root != NULL);
    ASSERT_EQ(make_dir(root, ".repo"), 0);
    ASSERT_EQ(write_relative(root, ".repo/manifest.xml",
        "<manifest><project name=\"escape\" path=\"../escape\"/></manifest>"), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_NEQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT(strstr(err, "unsafe") != NULL);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_manifest_keeps_duplicate_names_and_extend_path) {
    const char *temp_root = th_mktempdir("cbm_aosp_duplicate");
    ASSERT(temp_root != NULL);
    char *root = strdup(temp_root);
    ASSERT(root != NULL);
    ASSERT_EQ(make_dir(root, ".repo"), 0);
    ASSERT_EQ(make_dir(root, "vendor/one"), 0);
    ASSERT_EQ(make_dir(root, "vendor/two"), 0);
    ASSERT_EQ(write_relative(root, ".repo/manifest.xml",
        "<manifest>"
        "<project name=\"shared/project\" path=\"vendor/one\"/>"
        "<project name=\"shared/project\" path=\"vendor/two\"/>"
        "<extend-project name=\"shared/project\" path=\"vendor/two\" revision=\"refs/heads/x\"/>"
        "<remove-project name=\"shared/project\" path=\"vendor/one\"/>"
        "</manifest>"), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 1);
    ASSERT_STR_EQ(workspace.repos[0].path, "vendor/two");
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_workspace_ids_are_root_scoped) {
    char *left = NULL;
    char *right = NULL;
    ASSERT_EQ(create_workspace_fixture(&left), 0);
    ASSERT_EQ(create_workspace_fixture(&right), 0);
    cbm_aosp_workspace_t a;
    cbm_aosp_workspace_t b;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(left, &a, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_discover(right, &b, err, sizeof(err)), 0);
    ASSERT(strcmp(a.workspace_id, b.workspace_id) != 0);
    ASSERT_STR_EQ(a.manifest_hash, b.manifest_hash);
    ASSERT(strcmp(a.repos[0].repo_id, b.repos[0].repo_id) != 0);
    cbm_aosp_workspace_free(&a);
    cbm_aosp_workspace_free(&b);
    th_rmtree(left);
    th_rmtree(right);
    free(left);
    free(right);
    PASS();
}

TEST(aosp_master_sync_and_stats) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    cbm_aosp_master_stats_t stats;
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repo_count, 2);
    ASSERT_EQ(stats.existing_count, 2);
    ASSERT_EQ(stats.missing_count, 0);
    ASSERT_EQ(stats.indexed_count, 0);
    ASSERT_EQ(stats.cross_edge_count, 0);
    ASSERT_EQ(stats.stale_repo_count, 0);
    ASSERT_EQ(stats.refresh_failed_count, 0);

    char db_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, db_path, sizeof(db_path), false), 0);
    sqlite3 *db = NULL;
    ASSERT_EQ(sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN"
        "('repos','symbols','modules','cross_symbol_edges','architecture_summaries');",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 5);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_catalog_and_global_symbol_search) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_path[4096];
    (void)snprintf(shard_path, sizeof(shard_path), "%s/test-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(7,'HandleAudio','aosp.test.audio.HandleAudio','Function','media/audio.cpp',42,58,'{}'),"
        "(8,'audio.cpp','aosp.test.audio.__file__','File','media/audio.cpp',1,80,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);

    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], shard_path,
                                       err, sizeof(err)), 0);
    cbm_aosp_symbol_t *results = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_symbols(&workspace, "Handle", 10, &results, &count,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(results[0].name, "HandleAudio");
    ASSERT_STR_EQ(results[0].label, "Function");
    ASSERT_STR_EQ(results[0].repo_path, "frameworks/base");
    ASSERT_EQ((int)strlen(results[0].global_id), CBM_AOSP_HASH_LEN);
    ASSERT_STR_EQ(results[0].repo_id, workspace.repos[0].repo_id);
    ASSERT_EQ(results[0].local_node_id, 7);
    ASSERT(strncmp(results[0].project_name, "aosp-", 5) == 0);
    ASSERT_EQ(results[0].start_line, 42);
    cbm_aosp_symbols_free(results, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"Handle\",\"limit\":10}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_search_symbols", args);
    ASSERT(response != NULL);
    ASSERT(strstr(response, "\"isError\":false") != NULL);
    ASSERT(strstr(response, "HandleAudio") != NULL);
    ASSERT(strstr(response, "frameworks/base") != NULL);
    ASSERT(strstr(response, "global_id") != NULL);
    ASSERT(strstr(response, "repo_id") != NULL);
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_resolver_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'Unique','alpha.Unique','Class','alpha/Unique.java',10,30,'{}'),"
        "(2,'run','common.Service.run','Method','common/Service.java',40,50,'{}'),"
        "(3,'AliasExecute','vendor.deep.Service.execute','Method','vendor/Service.java',60,70,'{}'),"
        "(4,'Duplicate','one.Duplicate','Class','one/Duplicate.java',80,90,'{}'),"
        "(5,'ExactDuplicate','shared.ExactDuplicate','Class','shared/Exact.java',100,110,'{}'),"
        "(6,'call','native.api.Dispatch.call','Function','native/dispatch.cpp',120,130,'{}'),"
        "(7,'resolveSymbol','mixed.android::query.WorkspaceResolver.resolveSymbol',"
        "'Method','mixed/resolver.cpp',140,150,'{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'Other','beta.Other','Class','beta/Other.java',10,30,'{}'),"
        "(2,'run','vendor.common.Service.run','Method','vendor/Service.java',40,50,'{}'),"
        "(3,'Duplicate','two.Duplicate','Class','two/Duplicate.java',60,70,'{}'),"
        "(4,'ExactDuplicate','shared.ExactDuplicate','Class','shared/Exact.java',80,90,'{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_stmt *stmt = NULL;
    if (rc == 0 && repo_index == 1 &&
        sqlite3_prepare_v2(db,
            "INSERT INTO nodes VALUES(?1,'Crowded',?2,'Function','crowded.cpp',1,2,'{}');",
            -1, &stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < 205 && rc == 0; i++) {
            char qualified_name[128];
            (void)snprintf(qualified_name, sizeof(qualified_name),
                           "crowded.%03d.Crowded", i);
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_int(stmt, 1, 100 + i);
            sqlite3_bind_text(stmt, 2, qualified_name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        }
    } else if (rc == 0 && repo_index == 1) {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_workspace_symbol_resolver_tiers_and_ambiguity) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/resolver-%d.db", root, i);
        ASSERT_EQ(create_resolver_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
    }
    char master_path[4096];
    sqlite3 *master = NULL;
    sqlite3_stmt *schema_stmt = NULL;
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(master,
        "UPDATE symbols SET qualified_leaf='' "
        "WHERE qualified_name='vendor.deep.Service.execute';"
        "DELETE FROM schema_versions WHERE version=7;",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(master);
    master = NULL;
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    ASSERT_EQ(sqlite3_open_v2(master_path, &master, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT (SELECT count(*) FROM schema_versions WHERE version=7),"
        "(SELECT count(*) FROM symbols WHERE qualified_name='vendor.deep.Service.execute' "
        "AND qualified_leaf='execute');",
        -1, &schema_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(schema_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(schema_stmt, 0), 1);
    ASSERT_EQ(sqlite3_column_int(schema_stmt, 1), 1);
    sqlite3_finalize(schema_stmt);
    sqlite3_close(master);

    cbm_aosp_symbol_resolution_t resolution;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Unique", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_NAME);
    ASSERT_EQ(resolution.candidate_count, 1);
    ASSERT_EQ(resolution.total_candidate_count, 1);
    ASSERT_FALSE(resolution.truncated);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "alpha.Unique");
    ASSERT_STR_EQ(resolution.candidates[0].repo_id, workspace.repos[0].repo_id);
    ASSERT_EQ(resolution.candidates[0].local_node_id, 1);
    char *unique_global_id = strdup(resolution.candidates[0].global_id);
    ASSERT_NOT_NULL(unique_global_id);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, unique_global_id, &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_GLOBAL_ID);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "alpha.Unique");
    cbm_aosp_symbol_resolution_free(&resolution);
    free(unique_global_id);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "common.Service.run", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "common.Service.run");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "native::api::Dispatch::call", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "native.api.Dispatch.call");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(
        &workspace, "android::query::WorkspaceResolver::resolveSymbol", &resolution,
        err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name,
                  "mixed.android::query.WorkspaceResolver.resolveSymbol");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Service.execute", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_QUALIFIED_SUFFIX);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "vendor.deep.Service.execute");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Duplicate", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_AMBIGUOUS);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_NAME);
    ASSERT_EQ(resolution.candidate_count, 2);
    ASSERT_EQ(resolution.total_candidate_count, 2);
    cbm_aosp_symbol_resolution_free(&resolution);

    char mcp_args[8192];
    (void)snprintf(mcp_args, sizeof(mcp_args),
                   "{\"workspace_root\":\"%s\",\"reference\":\"Duplicate\"}", root);
    char *mcp_response = cbm_mcp_handle_tool(NULL, "aosp_resolve_symbol", mcp_args);
    ASSERT_NOT_NULL(mcp_response);
    ASSERT_NOT_NULL(strstr(mcp_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(mcp_response, "ambiguous"));
    ASSERT_NOT_NULL(strstr(mcp_response, "total_candidate_count"));
    free(mcp_response);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "shared.ExactDuplicate", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_AMBIGUOUS);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_QUALIFIED_NAME);
    ASSERT_EQ(resolution.candidate_count, 2);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "Crowded", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_AMBIGUOUS);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_EXACT_NAME);
    ASSERT_EQ(resolution.candidate_count, 200);
    ASSERT_EQ(resolution.total_candidate_count, 205);
    ASSERT_TRUE(resolution.truncated);
    ASSERT_STR_EQ(resolution.candidates[0].qualified_name, "crowded.000.Crowded");
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "wrong.Duplicate", &resolution,
                                      err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_NOT_FOUND);
    ASSERT_EQ(resolution.match_kind, CBM_AOSP_SYMBOL_MATCH_NONE);
    ASSERT_EQ(resolution.candidate_count, 0);
    cbm_aosp_symbol_resolution_free(&resolution);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_routing_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "CREATE TABLE edges(id INTEGER PRIMARY KEY,source_id INTEGER,target_id INTEGER,"
        "type TEXT,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'Unique','alpha.Unique','Class','alpha/Unique.java',10,30,'{}'),"
        "(2,'Helper','alpha.Helper','Class','alpha/Helper.java',40,50,'{}'),"
        "(3,'Service','alpha.Service','Class','alpha/Service.java',60,70,'{}');"
        "INSERT INTO edges VALUES"
        "(1,1,2,'CALLS','{}'),"
        "(2,2,3,'USES_TYPE','{\"note\":\"test\"}'),"
        "(3,3,1,'IMPLEMENTS','{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'Other','beta.Other','Class','beta/Other.java',10,30,'{}'),"
        "(2,'Consumer','beta.Consumer','Class','beta/Consumer.java',40,50,'{}');"
        "INSERT INTO edges VALUES"
        "(1,2,1,'CALLS','{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int mark_repo_indexed(const cbm_aosp_workspace_t *workspace, const char *repo_id,
                             const char *db_path) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "UPDATE repos SET status='indexed', db_path=?1 "
            "WHERE workspace_id=?2 AND repo_id=?3;",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, repo_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_shard_routing_routes_symbols_and_reads_nodes_and_edges) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/routing-%d.db", root, i);
        ASSERT_EQ(create_routing_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    /* Resolve a symbol to get its global_id via Q1 */
    cbm_aosp_symbol_resolution_t resolution;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Unique", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    ASSERT_EQ(resolution.candidate_count, 1);
    char *unique_global_id = strdup(resolution.candidates[0].global_id);
    ASSERT_NOT_NULL(unique_global_id);
    cbm_aosp_symbol_resolution_free(&resolution);

    /* Q2: Route by global_id to the owning shard */
    cbm_aosp_shard_route_t route;
    ASSERT_EQ(cbm_aosp_shard_route(&workspace, unique_global_id, &route, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(route.shard);
    ASSERT_STR_EQ(route.repo_id, workspace.repos[0].repo_id);
    ASSERT_EQ(route.local_node_id, 1);
    ASSERT_STR_EQ(route.global_id, unique_global_id);
    ASSERT_NOT_NULL(route.shard_path);
    ASSERT_STR_EQ(route.shard_path, shard_paths[0]);

    /* Read the routed node from the shard */
    cbm_aosp_symbol_t *node = calloc(1, sizeof(*node));
    ASSERT_NOT_NULL(node);
    ASSERT_EQ(cbm_aosp_shard_read_node(&route, node, err, sizeof(err)), 0);
    ASSERT_STR_EQ(node->name, "Unique");
    ASSERT_STR_EQ(node->qualified_name, "alpha.Unique");
    ASSERT_STR_EQ(node->label, "Class");
    ASSERT_STR_EQ(node->file_path, "alpha/Unique.java");
    ASSERT_EQ(node->start_line, 10);
    ASSERT_EQ(node->end_line, 30);
    ASSERT_EQ(node->local_node_id, 1);
    ASSERT_STR_EQ(node->repo_id, workspace.repos[0].repo_id);
    ASSERT_STR_EQ(node->global_id, unique_global_id);
    cbm_aosp_symbols_free(node, 1);

    /* Read outgoing edges: Unique -> Helper (CALLS) */
    cbm_aosp_shard_edge_t *out_edges = NULL;
    int out_count = 0;
    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_OUTGOING,
                                        &out_edges, &out_count, err, sizeof(err)), 0);
    ASSERT_EQ(out_count, 1);
    ASSERT_STR_EQ(out_edges[0].type, "CALLS");
    ASSERT_EQ(out_edges[0].source_id, 1);
    ASSERT_EQ(out_edges[0].target_id, 2);
    ASSERT_EQ(out_edges[0].neighbor_id, 2);
    ASSERT_STR_EQ(out_edges[0].neighbor_name, "Helper");
    ASSERT_STR_EQ(out_edges[0].neighbor_qualified_name, "alpha.Helper");
    ASSERT_STR_EQ(out_edges[0].neighbor_label, "Class");
    cbm_aosp_shard_edges_free(out_edges, out_count);

    /* Read incoming edges: Service -> Unique (IMPLEMENTS) */
    cbm_aosp_shard_edge_t *in_edges = NULL;
    int in_count = 0;
    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_INCOMING,
                                        &in_edges, &in_count, err, sizeof(err)), 0);
    ASSERT_EQ(in_count, 1);
    ASSERT_STR_EQ(in_edges[0].type, "IMPLEMENTS");
    ASSERT_EQ(in_edges[0].source_id, 3);
    ASSERT_EQ(in_edges[0].target_id, 1);
    ASSERT_EQ(in_edges[0].neighbor_id, 3);
    ASSERT_STR_EQ(in_edges[0].neighbor_name, "Service");
    ASSERT_STR_EQ(in_edges[0].neighbor_qualified_name, "alpha.Service");
    cbm_aosp_shard_edges_free(in_edges, in_count);
    cbm_aosp_shard_route_close(&route);

    /* Q2: Route by resolved symbol (skips Master global_id lookup) */
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Unique", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_shard_route_symbol(&workspace, resolution.candidates, &route,
                                          err, sizeof(err)), 0);
    ASSERT_NOT_NULL(route.shard);
    ASSERT_EQ(route.local_node_id, 1);
    ASSERT_STR_EQ(route.repo_id, workspace.repos[0].repo_id);
    cbm_aosp_shard_route_close(&route);
    cbm_aosp_symbol_resolution_free(&resolution);

    /* Q2: Route to repo 1 and verify cross-shard isolation */
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "beta.Consumer", &resolution, err, sizeof(err)), 0);
    ASSERT_EQ(resolution.status, CBM_AOSP_SYMBOL_RESOLVED);
    char *consumer_global_id = strdup(resolution.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&resolution);

    ASSERT_EQ(cbm_aosp_shard_route(&workspace, consumer_global_id, &route, err, sizeof(err)), 0);
    ASSERT_STR_EQ(route.repo_id, workspace.repos[1].repo_id);
    ASSERT_EQ(route.local_node_id, 2);
    ASSERT_STR_EQ(route.shard_path, shard_paths[1]);
    node = calloc(1, sizeof(*node));
    ASSERT_NOT_NULL(node);
    ASSERT_EQ(cbm_aosp_shard_read_node(&route, node, err, sizeof(err)), 0);
    ASSERT_STR_EQ(node->qualified_name, "beta.Consumer");
    cbm_aosp_symbols_free(node, 1);

    /* Consumer has outgoing CALLS to Other, no incoming edges */
    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_OUTGOING,
                                        &out_edges, &out_count, err, sizeof(err)), 0);
    ASSERT_EQ(out_count, 1);
    ASSERT_STR_EQ(out_edges[0].type, "CALLS");
    ASSERT_STR_EQ(out_edges[0].neighbor_qualified_name, "beta.Other");
    cbm_aosp_shard_edges_free(out_edges, out_count);

    ASSERT_EQ(cbm_aosp_shard_read_edges(&route, CBM_AOSP_SHARD_EDGE_INCOMING,
                                        &in_edges, &in_count, err, sizeof(err)), 0);
    ASSERT_EQ(in_count, 0);
    cbm_aosp_shard_route_close(&route);
    free(consumer_global_id);

    /* Q2: Missing global_id returns error */
    ASSERT_EQ(cbm_aosp_shard_route(&workspace, "nonexistent-global-id-000000",
                                   &route, err, sizeof(err)), -1);
    ASSERT(strstr(err, "not found") != NULL);

    /* Q2: Routing to a repo without indexed shard fails */
    {
        sqlite3 *master = NULL;
        char master_path[4096];
        ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
        ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
        sqlite3_stmt *ins = NULL;
        ASSERT_EQ(sqlite3_prepare_v2(master,
            "INSERT INTO symbols(global_id,workspace_id,repo_id,local_node_id,name,"
            "qualified_name,label,qualified_leaf,file_path,start_line,end_line,properties) "
            "VALUES('fake-orphan-id',?1,'orphan-repo',1,'Orphan','orphan.Orphan',"
            "'Class','Orphan','orphan.java',1,2,'{}');",
            -1, &ins, NULL), SQLITE_OK);
        sqlite3_bind_text(ins, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_finalize(ins);
        ASSERT_EQ(sqlite3_prepare_v2(master,
            "INSERT OR IGNORE INTO repos(repo_id,workspace_id,manifest_name,path,abs_path,"
            "status,generation) VALUES('orphan-repo',?1,'orphan','orphan','/orphan',"
            "'discovered','g');",
            -1, &ins, NULL), SQLITE_OK);
        sqlite3_bind_text(ins, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(ins), SQLITE_DONE);
        sqlite3_finalize(ins);
        sqlite3_close(master);
    }
    ASSERT_EQ(cbm_aosp_shard_route(&workspace, "fake-orphan-id", &route, err, sizeof(err)), -1);
    ASSERT(strstr(err, "not indexed") != NULL);

    free(unique_global_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_snippet_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'SnippetBase','snippet.SnippetBase','Function','src/Base.cpp',2,4,'{}'),"
        "(2,'SnippetEscape','snippet.SnippetEscape','Function','../outside.cpp',1,1,'{}'),"
        "(3,'SnippetRange','snippet.SnippetRange','Function','src/Short.cpp',1,3,'{}'),"
        "(4,'SnippetStale','snippet.SnippetStale','Function','src/Base.cpp',2,4,'{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'SnippetVendor','snippet.SnippetVendor','Function','src/Vendor.kt',2,4,'{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    if (rc == 0 && sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                                NULL, NULL, NULL) != SQLITE_OK) {
        rc = -1;
    }
    sqlite3_close(db);
    return rc;
}

TEST(aosp_workspace_search_routes_to_exact_source_snippets) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    ASSERT_EQ(make_dir(root, "frameworks/base/src"), 0);
    ASSERT_EQ(make_dir(root, "vendor/acme/widgets/src"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/Base.cpp",
                             "// header\nint SnippetBase() {\n  return 7;\n}\n// trailer\n"),
              0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/Short.cpp", "only one line\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/outside.cpp", "secret outside repo\n"), 0);
    ASSERT_EQ(write_relative(root, "vendor/acme/widgets/src/Vendor.kt",
                             "package demo\nfun SnippetVendor(): Int {\n    return 9\n}\n"),
              0);

    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/snippet-%d.db", root, i);
        ASSERT_EQ(create_snippet_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i], err,
                                           sizeof(err)),
                  0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    cbm_aosp_symbol_t *symbols = NULL;
    int symbol_count = 0;
    ASSERT_EQ(cbm_aosp_search_symbols(&workspace, "Snippet", 10, &symbols, &symbol_count, err,
                                      sizeof(err)),
              0);
    ASSERT_EQ(symbol_count, 5);
    char *escape_id = NULL;
    char *range_id = NULL;
    char *stale_id = NULL;
    char *base_id = NULL;
    char *vendor_id = NULL;
    int exact_count = 0;
    for (int i = 0; i < symbol_count; i++) {
        cbm_aosp_source_snippet_t snippet;
        if (strcmp(symbols[i].name, "SnippetBase") == 0) {
            ASSERT_EQ(cbm_aosp_read_source_snippet(&workspace, symbols[i].global_id, &snippet, err,
                                                   sizeof(err)),
                      0);
            ASSERT_STR_EQ(snippet.symbol.repo_path, "frameworks/base");
            ASSERT_STR_EQ(snippet.symbol.file_path, "src/Base.cpp");
            ASSERT_EQ(snippet.symbol.start_line, 2);
            ASSERT_EQ(snippet.symbol.end_line, 4);
            ASSERT_STR_EQ(snippet.workspace_file_path, "frameworks/base/src/Base.cpp");
            ASSERT_STR_EQ(snippet.source, "int SnippetBase() {\n  return 7;\n}\n");
            ASSERT_NOT_NULL(snippet.absolute_file_path);
            base_id = strdup(symbols[i].global_id);
            ASSERT_NOT_NULL(base_id);
            cbm_aosp_source_snippet_free(&snippet);
            exact_count++;
        } else if (strcmp(symbols[i].name, "SnippetVendor") == 0) {
            ASSERT_EQ(cbm_aosp_read_source_snippet(&workspace, symbols[i].global_id, &snippet, err,
                                                   sizeof(err)),
                      0);
            ASSERT_STR_EQ(snippet.symbol.repo_path, "vendor/acme/widgets");
            ASSERT_STR_EQ(snippet.workspace_file_path, "vendor/acme/widgets/src/Vendor.kt");
            ASSERT_STR_EQ(snippet.source, "fun SnippetVendor(): Int {\n    return 9\n}\n");
            vendor_id = strdup(symbols[i].global_id);
            ASSERT_NOT_NULL(vendor_id);
            cbm_aosp_source_snippet_free(&snippet);
            exact_count++;
        } else if (strcmp(symbols[i].name, "SnippetEscape") == 0) {
            escape_id = strdup(symbols[i].global_id);
        } else if (strcmp(symbols[i].name, "SnippetRange") == 0) {
            range_id = strdup(symbols[i].global_id);
        } else if (strcmp(symbols[i].name, "SnippetStale") == 0) {
            stale_id = strdup(symbols[i].global_id);
        }
    }
    ASSERT_EQ(exact_count, 2);
    ASSERT_NOT_NULL(escape_id);
    ASSERT_NOT_NULL(range_id);
    ASSERT_NOT_NULL(stale_id);
    ASSERT_NOT_NULL(base_id);
    ASSERT_NOT_NULL(vendor_id);

    char mcp_args[8192];
    (void)snprintf(mcp_args, sizeof(mcp_args),
                   "{\"workspace_root\":\"%s\",\"reference\":\"snippet.SnippetBase\"}",
                   root);
    char *mcp_response = cbm_mcp_handle_tool(NULL, "aosp_resolve_symbol", mcp_args);
    ASSERT_NOT_NULL(mcp_response);
    ASSERT_NOT_NULL(strstr(mcp_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(mcp_response, "resolved"));
    ASSERT_NOT_NULL(strstr(mcp_response, base_id));
    free(mcp_response);

    (void)snprintf(mcp_args, sizeof(mcp_args),
                   "{\"workspace_root\":\"%s\",\"global_id\":\"%s\"}",
                   root, base_id);
    mcp_response = cbm_mcp_handle_tool(NULL, "aosp_get_source_snippet", mcp_args);
    ASSERT_NOT_NULL(mcp_response);
    ASSERT_NOT_NULL(strstr(mcp_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(mcp_response, "frameworks/base/src/Base.cpp"));
    ASSERT_NOT_NULL(strstr(mcp_response, "return 7"));
    free(mcp_response);

    cbm_aosp_source_snippet_t snippet;
    err[0] = '\0';
    ASSERT_NEQ(
        cbm_aosp_read_source_snippet(&workspace, "missing-global-id", &snippet, err, sizeof(err)),
        0);
    ASSERT_NOT_NULL(strstr(err, "not found"));

    err[0] = '\0';
    ASSERT_NEQ(cbm_aosp_read_source_snippet(&workspace, escape_id, &snippet, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "escapes repository root"));

    err[0] = '\0';
    ASSERT_NEQ(cbm_aosp_read_source_snippet(&workspace, range_id, &snippet, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "range exceeds"));

    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_paths[0], &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
                           "UPDATE nodes SET qualified_name='changed.SnippetStale' WHERE id=4;",
                           NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(shard);
    err[0] = '\0';
    ASSERT_NEQ(cbm_aosp_read_source_snippet(&workspace, stale_id, &snippet, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "catalog is stale"));

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "UPDATE repos SET status='discovered',db_path='' "
                  "WHERE workspace_id=?1 AND repo_id=?2;",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[1].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(master);
    err[0] = '\0';
    ASSERT_NEQ(cbm_aosp_read_source_snippet(&workspace, vendor_id, &snippet, err, sizeof(err)), 0);
    ASSERT_NOT_NULL(strstr(err, "not indexed"));

    free(escape_id);
    free(range_id);
    free(stale_id);
    free(base_id);
    free(vendor_id);
    cbm_aosp_symbols_free(symbols, symbol_count);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_trace_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "CREATE TABLE edges(id INTEGER PRIMARY KEY,source_id INTEGER,target_id INTEGER,"
        "type TEXT,properties TEXT);";
    const char *repo_zero =
        "INSERT INTO nodes VALUES"
        "(1,'Start','alpha.Start','Class','alpha/Start.java',10,20,'{}'),"
        "(2,'Mid','alpha.Mid','Class','alpha/Mid.java',30,40,'{}');"
        "INSERT INTO edges VALUES(1,1,2,'CALLS','{}');";
    const char *repo_one =
        "INSERT INTO nodes VALUES"
        "(1,'Target','beta.Target','Class','beta/Target.java',10,20,'{}'),"
        "(2,'Deep','beta.Deep','Class','beta/Deep.java',30,40,'{}');"
        "INSERT INTO edges VALUES(1,1,2,'CALLS','{}');";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, repo_index == 0 ? repo_zero : repo_one,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int create_q7_shard(const char *path, int repo_index) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "CREATE TABLE edges(id INTEGER PRIMARY KEY,source_id INTEGER,target_id INTEGER,"
        "type TEXT,properties TEXT);";
    const char *rows[] = {
        "INSERT INTO nodes VALUES"
        "(1,'Q7Start','q7.alpha.Q7Start','Function','src/Alpha.c',1,3,'{}'),"
        "(2,'Q7Local','q7.alpha.Q7Local','Function','src/Alpha.c',5,7,'{}'),"
        "(3,'Duplicate','q7.alpha.Duplicate','Function','src/Alpha.c',9,11,'{}');"
        "INSERT INTO edges VALUES(1,1,2,'CALLS','{}');",
        "INSERT INTO nodes VALUES"
        "(1,'Q7Bridge','q7.beta.Q7Bridge','Function','src/Beta.c',1,3,'{}'),"
        "(2,'Duplicate','q7.beta.Duplicate','Function','src/Beta.c',5,7,'{}'),"
        "(3,'Q7BetaLocal','q7.beta.Q7BetaLocal','Function','src/Beta.c',9,11,'{}');"
        "INSERT INTO edges VALUES(1,1,3,'CALLS','{}');",
        "INSERT INTO nodes VALUES"
        "(1,'Q7End','q7.gamma.Q7End','Function','src/Gamma.c',1,3,'{}');",
    };
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK &&
             repo_index >= 0 && repo_index < 3 &&
             sqlite3_exec(db, rows[repo_index], NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

static int insert_cross_edge(const cbm_aosp_workspace_t *workspace,
                             const char *edge_id, const char *source_repo_id,
                             const char *target_repo_id, const char *source_global_id,
                             const char *target_global_id, const char *type,
                             double confidence, const char *evidence) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO cross_symbol_edges(edge_id,workspace_id,source_repo_id,target_repo_id,"
            "source_global_id,target_global_id,target_name,target_leaf,type,status,"
            "confidence,evidence,source_generation,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,'','',?7,'resolved',?8,?9,'','{}');",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, target_repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, source_global_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, target_global_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, confidence);
    sqlite3_bind_text(stmt, 9, evidence, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_trace_path_traverses_local_and_cross_repo_edges) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/trace-%d.db", root, i);
        ASSERT_EQ(create_trace_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    /* Resolve symbols to get global IDs */
    cbm_aosp_symbol_resolution_t res;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Start", &res, err, sizeof(err)), 0);
    ASSERT_EQ(res.candidate_count, 1);
    char *start_gid = strdup(res.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&res);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Mid", &res, err, sizeof(err)), 0);
    char *mid_gid = strdup(res.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&res);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "beta.Target", &res, err, sizeof(err)), 0);
    char *target_gid = strdup(res.candidates[0].global_id);
    char *target_repo_id = strdup(res.candidates[0].repo_id);
    cbm_aosp_symbol_resolution_free(&res);

    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "beta.Deep", &res, err, sizeof(err)), 0);
    char *deep_gid = strdup(res.candidates[0].global_id);
    cbm_aosp_symbol_resolution_free(&res);

    /* Insert cross-repository edges:
     *   alpha.Start --CALLS--> beta.Target (confidence 0.9)
     *   alpha.Mid   --USES_TYPE--> beta.Deep (confidence 0.8) */
    ASSERT_EQ(insert_cross_edge(&workspace, "edge1", workspace.repos[0].repo_id,
                                target_repo_id, start_gid, target_gid, "CALLS", 0.9,
                                "cross_calls"), 0);
    ASSERT_EQ(insert_cross_edge(&workspace, "edge2", workspace.repos[0].repo_id,
                                target_repo_id, mid_gid, deep_gid, "USES_TYPE", 0.8,
                                "cross_uses"), 0);

    /* Test: full outgoing traversal from alpha.Start
     * Graph: Start -> Mid (local), Start -> Target (cross), Mid -> Deep (cross),
     *        Target -> Deep (local, but Deep already visited via cross) */
    cbm_aosp_trace_options_t opts = {0};
    opts.max_depth = -1;
    opts.direction = CBM_AOSP_TRACE_OUTGOING;
    opts.result_budget = 0;
    opts.cancel_flag = NULL;

    cbm_aosp_trace_result_t result;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 4);
    ASSERT_FALSE(result.truncated);
    ASSERT_EQ(result.max_depth_reached, 2);
    /* Start node */
    ASSERT_STR_EQ(result.nodes[0].global_id, start_gid);
    ASSERT_EQ(result.nodes[0].depth, 0);
    ASSERT_FALSE(result.nodes[0].cross_repo);
    ASSERT(result.nodes[0].edge_type == NULL);
    /* Depth 1 nodes: Mid (local) and Target (cross) */
    bool found_mid = false, found_target = false;
    for (int i = 1; i <= 2; i++) {
        if (strcmp(result.nodes[i].global_id, mid_gid) == 0) {
            found_mid = true;
            ASSERT_EQ(result.nodes[i].depth, 1);
            ASSERT_FALSE(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "CALLS");
            ASSERT_EQ(result.nodes[i].confidence, 1.0);
        }
        if (strcmp(result.nodes[i].global_id, target_gid) == 0) {
            found_target = true;
            ASSERT_EQ(result.nodes[i].depth, 1);
            ASSERT(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "CALLS");
            ASSERT_EQ(result.nodes[i].confidence, 0.9);
        }
    }
    ASSERT(found_mid);
    ASSERT(found_target);
    /* Depth 2: Deep (reached via cross from Mid or local from Target) */
    ASSERT_STR_EQ(result.nodes[3].global_id, deep_gid);
    ASSERT_EQ(result.nodes[3].depth, 2);
    cbm_aosp_trace_result_free(&result);

    /* Test: max_depth=0 returns only start */
    opts.max_depth = 0;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 1);
    ASSERT_EQ(result.max_depth_reached, 0);
    cbm_aosp_trace_result_free(&result);

    /* Test: max_depth=1 returns start + depth 1 */
    opts.max_depth = 1;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 3);
    ASSERT_EQ(result.max_depth_reached, 1);
    cbm_aosp_trace_result_free(&result);

    /* Test: result_budget=2 truncates */
    opts.max_depth = -1;
    opts.result_budget = 2;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 2);
    ASSERT(result.truncated);
    cbm_aosp_trace_result_free(&result);

    /* Test: incoming direction from beta.Deep finds:
     *   Depth 0: Deep
     *   Depth 1: Target (local CALLS), Mid (cross USES_TYPE)
     *   Depth 2: Start (via Target cross Start→Target, or Mid local Start→Mid) */
    opts.max_depth = -1;
    opts.direction = CBM_AOSP_TRACE_INCOMING;
    opts.result_budget = 0;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, deep_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 4);
    /* Start: Deep */
    ASSERT_STR_EQ(result.nodes[0].global_id, deep_gid);
    ASSERT_EQ(result.nodes[0].depth, 0);
    /* Depth 1: Target (local CALLS) and Mid (cross USES_TYPE) */
    bool found_target_in = false, found_mid_in = false;
    for (int i = 1; i <= 2; i++) {
        if (strcmp(result.nodes[i].global_id, target_gid) == 0) {
            found_target_in = true;
            ASSERT_FALSE(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "CALLS");
        }
        if (strcmp(result.nodes[i].global_id, mid_gid) == 0) {
            found_mid_in = true;
            ASSERT(result.nodes[i].cross_repo);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "USES_TYPE");
        }
    }
    ASSERT(found_target_in);
    ASSERT(found_mid_in);
    cbm_aosp_trace_result_free(&result);

    char mcp_args[8192];
    (void)snprintf(
        mcp_args, sizeof(mcp_args),
        "{\"workspace_root\":\"%s\",\"start\":\"alpha.Start\","
        "\"max_depth\":2,\"direction\":\"outgoing\",\"result_budget\":10}",
        root);
    char *mcp_response = cbm_mcp_handle_tool(NULL, "aosp_trace_path", mcp_args);
    ASSERT_NOT_NULL(mcp_response);
    ASSERT_NOT_NULL(strstr(mcp_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(mcp_response, target_gid));
    ASSERT_NOT_NULL(strstr(mcp_response, "cross_calls"));
    ASSERT_NOT_NULL(strstr(mcp_response, "cross_repo"));
    free(mcp_response);

    /* Test: missing start symbol returns error */
    opts.direction = CBM_AOSP_TRACE_OUTGOING;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, "nonexistent-id", &opts, &result, err, sizeof(err)),
              -1);
    ASSERT(strstr(err, "not found") != NULL);

    free(start_gid);
    free(mid_gid);
    free(target_gid);
    free(target_repo_id);
    free(deep_gid);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int insert_module(const cbm_aosp_workspace_t *workspace, const char *module_id,
                         const char *repo_id, const char *name, const char *type,
                         const char *file_path) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO modules(module_id,workspace_id,repo_id,name,module_type,file_path,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, module_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, file_path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int insert_module_dep(const cbm_aosp_workspace_t *workspace,
                             const char *source_id, const char *target_name,
                             const char *target_id, const char *type) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO module_dependencies(source_id,target_name,type,target_id,resolved,properties) "
            "VALUES(?1,?2,?3,?4,1,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, target_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int insert_protocol_node(const cbm_aosp_workspace_t *workspace,
                                const char *protocol_id, const char *repo_id,
                                const char *kind, const char *name,
                                const char *qualified_name, const char *file_path,
                                const char *symbol_global_id) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO protocol_nodes(protocol_id,workspace_id,repo_id,kind,name,"
            "qualified_name,file_path,symbol_global_id,properties) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, protocol_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, repo_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, qualified_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, file_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, symbol_global_id ? symbol_global_id : "", -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static int insert_protocol_edge(const cbm_aosp_workspace_t *workspace,
                                const char *source_id, const char *target_id,
                                const char *type, double confidence,
                                const char *evidence) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_open(master_path, &db) != SQLITE_OK) goto done;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO protocol_edges(source_id,target_id,type,confidence,evidence,properties) "
            "VALUES(?1,?2,?3,?4,?5,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, confidence);
    sqlite3_bind_text(stmt, 5, evidence, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_query_graph_traverses_multi_hop_code_module_protocol) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_paths[2][4096];
    for (int i = 0; i < 2; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/qg-%d.db", root, i);
        ASSERT_EQ(create_trace_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    /* Resolve start symbol */
    cbm_aosp_symbol_resolution_t res;
    ASSERT_EQ(cbm_aosp_resolve_symbol(&workspace, "alpha.Start", &res, err, sizeof(err)), 0);
    char *start_gid = strdup(res.candidates[0].global_id);
    char *start_repo_id = strdup(res.candidates[0].repo_id);
    cbm_aosp_symbol_resolution_free(&res);

    /* Insert modules: one in repo 0 (Start's and Mid's file), one in repo 1 */
    ASSERT_EQ(insert_module(&workspace, "mod-alpha", start_repo_id,
                            "libalpha", "cc_library_shared", "alpha/Start.java"), 0);
    ASSERT_EQ(insert_module(&workspace, "mod-alpha-mid", start_repo_id,
                            "libalpha_mid", "cc_library_shared", "alpha/Mid.java"), 0);
    ASSERT_EQ(insert_module(&workspace, "mod-beta", workspace.repos[1].repo_id,
                            "libbeta", "cc_library_shared", "beta/Target.java"), 0);
    /* Module dependency: mod-alpha-mid depends on mod-beta */
    ASSERT_EQ(insert_module_dep(&workspace, "mod-alpha-mid", "libbeta", "mod-beta",
                                "shared_libs"), 0);

    /* Insert protocol node linked to Start symbol */
    ASSERT_EQ(insert_protocol_node(&workspace, "proto-start", start_repo_id,
                                   "AIDL", "IStart", "alpha.IStart",
                                   "alpha/Start.java", start_gid), 0);
    ASSERT_EQ(insert_protocol_node(&workspace, "proto-target", workspace.repos[1].repo_id,
                                   "AIDL", "ITarget", "beta.ITarget",
                                   "beta/Target.java", NULL), 0);
    /* Protocol edge: proto-start implements proto-target */
    ASSERT_EQ(insert_protocol_edge(&workspace, "proto-start", "proto-target",
                                   "AIDL_PROXY", 0.9, "aidl_link"), 0);

    /* Test 1: Single-hop SYMBOL query (same as trace_path with depth 1) */
    cbm_aosp_query_hop_t code_hop = {0};
    code_hop.kind = CBM_AOSP_QUERY_KIND_SYMBOL;
    code_hop.direction = CBM_AOSP_TRACE_OUTGOING;
    code_hop.edge_type = NULL;

    cbm_aosp_query_graph_options_t opts = {0};
    opts.hops = &code_hop;
    opts.hop_count = 1;
    opts.max_results = 0;

    cbm_aosp_query_graph_result_t result;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    /* Start + alpha.Mid (local CALLS) = 2 nodes */
    ASSERT_EQ(result.node_count, 2);
    ASSERT_EQ(result.nodes[0].hop_index, 0);
    ASSERT_EQ(result.nodes[0].kind, CBM_AOSP_QUERY_KIND_SYMBOL);
    ASSERT_STR_EQ(result.nodes[0].name, "Start");
    ASSERT_EQ(result.nodes[1].hop_index, 1);
    ASSERT_EQ(result.nodes[1].kind, CBM_AOSP_QUERY_KIND_SYMBOL);
    ASSERT_STR_EQ(result.nodes[1].name, "Mid");
    ASSERT_STR_EQ(result.nodes[1].edge_type, "CALLS");
    ASSERT_STR_EQ(result.nodes[1].edge_evidence, "local");
    cbm_aosp_query_graph_result_free(&result);

    /* Test 2: SYMBOL → MODULE pattern (find containing module) */
    cbm_aosp_query_hop_t hops2[2] = {0};
    hops2[0].kind = CBM_AOSP_QUERY_KIND_SYMBOL;
    hops2[0].direction = CBM_AOSP_TRACE_OUTGOING;
    hops2[1].kind = CBM_AOSP_QUERY_KIND_MODULE;
    hops2[1].direction = CBM_AOSP_TRACE_BOTH;

    opts.hops = hops2;
    opts.hop_count = 2;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    /* Start (SYMBOL, hop 0), alpha.Mid (SYMBOL, hop 1), mod-alpha-mid (MODULE, hop 2) */
    ASSERT(result.node_count >= 3);
    bool found_module = false;
    for (int i = 0; i < result.node_count; i++) {
        if (result.nodes[i].kind == CBM_AOSP_QUERY_KIND_MODULE) {
            found_module = true;
            ASSERT_STR_EQ(result.nodes[i].node_id, "mod-alpha-mid");
            ASSERT_EQ(result.nodes[i].hop_index, 2);
        }
    }
    ASSERT(found_module);
    cbm_aosp_query_graph_result_free(&result);

    /* Test 3: SYMBOL → MODULE → MODULE pattern (follow module deps) */
    cbm_aosp_query_hop_t hops3[3] = {0};
    hops3[0].kind = CBM_AOSP_QUERY_KIND_SYMBOL;
    hops3[0].direction = CBM_AOSP_TRACE_OUTGOING;
    hops3[1].kind = CBM_AOSP_QUERY_KIND_MODULE;
    hops3[1].direction = CBM_AOSP_TRACE_BOTH;
    hops3[2].kind = CBM_AOSP_QUERY_KIND_MODULE;
    hops3[2].direction = CBM_AOSP_TRACE_OUTGOING;

    opts.hops = hops3;
    opts.hop_count = 3;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    /* Should find mod-beta via mod-alpha's dependency */
    bool found_beta = false;
    for (int i = 0; i < result.node_count; i++) {
        if (result.nodes[i].kind == CBM_AOSP_QUERY_KIND_MODULE &&
            strcmp(result.nodes[i].node_id, "mod-beta") == 0) {
            found_beta = true;
            ASSERT_EQ(result.nodes[i].hop_index, 3);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "depends_on");
            ASSERT_STR_EQ(result.nodes[i].edge_evidence, "module_dependencies");
        }
    }
    ASSERT(found_beta);
    cbm_aosp_query_graph_result_free(&result);

    /* Test 4: SYMBOL → PROTOCOL pattern (find linked protocol node from Start) */
    cbm_aosp_query_hop_t hops4[2] = {0};
    hops4[0].kind = CBM_AOSP_QUERY_KIND_PROTOCOL;
    hops4[0].direction = CBM_AOSP_TRACE_BOTH;
    hops4[1].kind = CBM_AOSP_QUERY_KIND_PROTOCOL;
    hops4[1].direction = CBM_AOSP_TRACE_OUTGOING;

    opts.hops = hops4;
    opts.hop_count = 1;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    /* Should find proto-start linked to Start symbol */
    bool found_proto = false;
    for (int i = 0; i < result.node_count; i++) {
        if (result.nodes[i].kind == CBM_AOSP_QUERY_KIND_PROTOCOL) {
            found_proto = true;
            ASSERT_STR_EQ(result.nodes[i].node_id, "proto-start");
            ASSERT_EQ(result.nodes[i].hop_index, 1);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "protocol_link");
        }
    }
    ASSERT(found_proto);
    cbm_aosp_query_graph_result_free(&result);

    /* Test 5: PROTOCOL → PROTOCOL (follow protocol edges) */
    cbm_aosp_query_hop_t hops5[2] = {0};
    hops5[0].kind = CBM_AOSP_QUERY_KIND_PROTOCOL;
    hops5[0].direction = CBM_AOSP_TRACE_BOTH;
    hops5[1].kind = CBM_AOSP_QUERY_KIND_PROTOCOL;
    hops5[1].direction = CBM_AOSP_TRACE_OUTGOING;

    opts.hops = hops5;
    opts.hop_count = 2;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    /* Should find proto-target via proto-start's AIDL_PROXY edge */
    bool found_proto_target = false;
    for (int i = 0; i < result.node_count; i++) {
        if (result.nodes[i].kind == CBM_AOSP_QUERY_KIND_PROTOCOL &&
            strcmp(result.nodes[i].node_id, "proto-target") == 0) {
            found_proto_target = true;
            ASSERT_EQ(result.nodes[i].hop_index, 2);
            ASSERT_STR_EQ(result.nodes[i].edge_type, "AIDL_PROXY");
            ASSERT_STR_EQ(result.nodes[i].edge_evidence, "aidl_link");
            ASSERT_EQ(result.nodes[i].confidence, 0.9);
        }
    }
    ASSERT(found_proto_target);
    cbm_aosp_query_graph_result_free(&result);

    char mcp_args[8192];
    (void)snprintf(
        mcp_args, sizeof(mcp_args),
        "{\"workspace_root\":\"%s\",\"start\":\"alpha.Start\",\"hops\":["
        "{\"kind\":\"symbol\",\"direction\":\"outgoing\"},"
        "{\"kind\":\"module\",\"direction\":\"both\"},"
        "{\"kind\":\"module\",\"direction\":\"outgoing\"}],\"max_results\":20}",
        root);
    char *mcp_response = cbm_mcp_handle_tool(NULL, "aosp_query_graph", mcp_args);
    ASSERT_NOT_NULL(mcp_response);
    ASSERT_NOT_NULL(strstr(mcp_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(mcp_response, "mod-beta"));
    ASSERT_NOT_NULL(strstr(mcp_response, "depends_on"));
    ASSERT_NOT_NULL(strstr(mcp_response, "module_dependencies"));
    free(mcp_response);

    /* Test 6: Result budget truncation */
    opts.hops = &code_hop;
    opts.hop_count = 1;
    opts.max_results = 1;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_gid, &opts, &result, err, sizeof(err)), 0);
    ASSERT_EQ(result.node_count, 1);
    ASSERT(result.truncated);
    cbm_aosp_query_graph_result_free(&result);

    /* Test 7: Missing start symbol */
    opts.hops = &code_hop;
    opts.hop_count = 1;
    opts.max_results = 0;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, "nonexistent", &opts, &result, err, sizeof(err)),
              -1);
    ASSERT(strstr(err, "not found") != NULL);

    free(start_gid);
    free(start_repo_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_resolves_cross_repo_modules) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.blueprint_files, 2);
    ASSERT_EQ(stats.make_files, 1);
    ASSERT_EQ(stats.aidl_files, 1);
    ASSERT_EQ(stats.module_count, 7);
    ASSERT_EQ(stats.dependency_count, 6);
    ASSERT_EQ(stats.resolved_count, 5);
    ASSERT_EQ(stats.unresolved_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(dependency,',') FROM ("
                  "SELECT d.target_name||':'||d.type AS dependency "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='libframework_audio' "
                  "ORDER BY d.target_name,d.type);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "libandroid_extra:SHARED_LIB,libbase_defaults:STATIC_LIB,"
                  "libmissing:SHARED_LIB,libvendor_audio:SHARED_LIB");
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    cbm_aosp_module_t *modules = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_modules(&workspace, "libframework_audio", 10, &modules,
                                      &count, err, sizeof(err)), 0);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(modules[0].repo_path, "frameworks/base");
    ASSERT_EQ(modules[0].outgoing_dependencies, 3);
    cbm_aosp_modules_free(modules, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"libframework_audio\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT(response != NULL);
    ASSERT(strstr(response, "\"isError\":false") != NULL);
    ASSERT(strstr(response, "libframework_audio") != NULL);
    ASSERT(strstr(response, "dependencies_unresolved") != NULL);
    ASSERT(strstr(response, "\"dependencies\"") != NULL);
    ASSERT(strstr(response, "libmissing") != NULL);
    ASSERT(strstr(response, "\"resolved\":false") != NULL);
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_expands_defaults_with_provenance_and_cycles) {
    char *root = NULL;
    ASSERT_EQ(create_defaults_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.module_count, 12);
    ASSERT_EQ(stats.dependency_count, 21);
    ASSERT_EQ(stats.resolved_count, 21);
    ASSERT_EQ(stats.unresolved_count, 0);
    ASSERT_EQ(stats.inherited_dependency_count, 9);
    ASSERT_EQ(stats.defaults_cycle_count, 3);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(d.properties,'$.origin'),"
                  "json_extract(d.properties,'$.inherited_from'),"
                  "json_extract(d.properties,'$.inheritance_path'),"
                  "json_extract(d.properties,'$.inheritance_depth') "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='libconsumer' AND d.target_name='libbase';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "defaults");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "base_defaults");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "mid_defaults>base_defaults");
    ASSERT_EQ(sqlite3_column_int(stmt, 3), 2);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(d.properties,'$.origin') "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='libconsumer' AND d.target_name='libdirect';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "direct");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(name,',') FROM (SELECT name FROM modules "
                  "WHERE workspace_id=?1 AND json_extract(properties,'$.defaults_cycle')="
                  "'cycle_a>cycle_b>cycle_a' ORDER BY name);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "cycle_a,cycle_b,libcycle_consumer");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(e.properties,'$.inheritance_path') "
                  "FROM module_edges e JOIN modules m ON m.module_id=e.source_id "
                  "JOIN modules t ON t.module_id=e.target_id "
                  "WHERE m.workspace_id=?1 AND m.name='libconsumer' AND t.name='libbase';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "mid_defaults>base_defaults");
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"libconsumer\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"dependencies_inherited\":9"));
    ASSERT_NOT_NULL(strstr(response, "\"defaults_cycles\":3"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_models_conditional_variants) {
    char *root = NULL;
    ASSERT_EQ(create_variants_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.module_count, 14);
    ASSERT_EQ(stats.dependency_count, 14);
    ASSERT_EQ(stats.resolved_count, 14);
    ASSERT_EQ(stats.unresolved_count, 0);
    ASSERT_EQ(stats.inherited_dependency_count, 1);
    ASSERT_EQ(stats.defaults_cycle_count, 0);
    ASSERT_EQ(stats.variant_dependency_count, 13);
    ASSERT_EQ(stats.variant_branch_count, 14);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.compile_multilib') FROM modules "
                  "WHERE workspace_id=?1 AND name='libvariant_consumer';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "both");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT j.value FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id, "
                  "json_each(d.properties,'$.variants') j WHERE m.workspace_id=?1 "
                  "AND m.name='libvariant_consumer' AND d.target_name='libselect';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "select.soong_config_variable.acme.mode.enabled");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(value,',') FROM ("
                  "SELECT j.value FROM module_dependencies d "
                  "JOIN modules m ON m.module_id=d.source_id, json_each(d.properties,'$.variants') j "
                  "WHERE m.workspace_id=?1 AND m.name='libvariant_consumer' "
                  "AND d.target_name='libdup' ORDER BY j.value);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "arch.arm64,target.android");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(d.properties,'$.origin'),"
                  "json_extract(d.properties,'$.unconditional'),j.value "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id, "
                  "json_each(d.properties,'$.variants') j "
                  "WHERE m.workspace_id=?1 AND m.name='libvariant_consumer' "
                  "AND d.target_name='libfromdefaults';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "defaults");
    ASSERT_FALSE(sqlite3_column_int(stmt, 1));
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2),
                  "target.android&arch.arm64");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(d.properties,'$.unconditional'),"
                  "json_array_length(d.properties,'$.variants') "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='libvariant_consumer' "
                  "AND d.target_name='libcommon';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_TRUE(sqlite3_column_int(stmt, 0));
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT j.value FROM module_edges e JOIN modules m ON m.module_id=e.source_id "
                  "JOIN modules t ON t.module_id=e.target_id, json_each(e.properties,'$.variants') j "
                  "WHERE m.workspace_id=?1 AND m.name='libvariant_consumer' "
                  "AND t.name='libfromdefaults';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "target.android&arch.arm64");
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"libvariant_consumer\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"variant_dependencies\":13"));
    ASSERT_NOT_NULL(strstr(response, "\"variant_branches\":14"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_models_namespaces_packages_and_visibility) {
    char *root = NULL;
    ASSERT_EQ(create_build_boundaries_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.blueprint_files, 5);
    ASSERT_EQ(stats.module_count, 13);
    ASSERT_EQ(stats.dependency_count, 11);
    ASSERT_EQ(stats.resolved_count, 6);
    ASSERT_EQ(stats.unresolved_count, 5);
    ASSERT_EQ(stats.namespace_count, 3);
    ASSERT_EQ(stats.namespace_import_count, 2);
    ASSERT_EQ(stats.package_count, 5);
    ASSERT_EQ(stats.ambiguous_dependency_count, 1);
    ASSERT_EQ(stats.visibility_blocked_count, 2);
    ASSERT_EQ(stats.unsupported_visibility_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(namespace_path,',') FROM ("
                  "SELECT namespace_path FROM build_namespaces WHERE workspace_id=?1 "
                  "ORDER BY namespace_path);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "app/client,vendor/one,vendor/two");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT imports FROM build_namespaces WHERE workspace_id=?1 "
                  "AND namespace_path='app/client';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "[\"vendor/one\",\"vendor/two\"]");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.package'),"
                  "json_extract(properties,'$.namespace'),"
                  "json_extract(properties,'$.visibility_origin'),"
                  "json_extract(properties,'$.visibility_source_package'),"
                  "json_extract(properties,'$.visibility[0]') FROM modules "
                  "WHERE workspace_id=?1 AND name='libsubconsumer';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "app/client/sub");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "app/client");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "package_default");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "app/client");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4), "//visibility:private");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT d.resolved,json_extract(t.properties,'$.namespace'),"
                  "json_extract(d.properties,'$.resolution') "
                  "FROM module_dependencies d JOIN modules s ON s.module_id=d.source_id "
                  "LEFT JOIN modules t ON t.module_id=d.target_id "
                  "WHERE s.workspace_id=?1 AND s.name='libconsumer' "
                  "AND d.target_name='libsame';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_TRUE(sqlite3_column_int(stmt, 0));
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "app/client");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "current_namespace");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT resolved,json_extract(properties,'$.failure_reason'),"
                  "json_extract(properties,'$.candidate_count'),"
                  "json_extract(properties,'$.resolution') FROM module_dependencies "
                  "WHERE source_id=(SELECT module_id FROM modules WHERE workspace_id=?1 "
                  "AND name='libconsumer') AND target_name='libduplicate';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_FALSE(sqlite3_column_int(stmt, 0));
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "ambiguous");
    ASSERT_EQ(sqlite3_column_int(stmt, 2), 2);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "imported_namespace");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.failure_reason'),"
                  "json_extract(properties,'$.target_namespace') FROM module_dependencies "
                  "WHERE source_id=(SELECT module_id FROM modules WHERE workspace_id=?1 "
                  "AND name='libconsumer') AND target_name='libprivate';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "visibility_blocked");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "vendor/one");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.failure_reason') FROM module_dependencies "
                  "WHERE source_id=(SELECT module_id FROM modules WHERE workspace_id=?1 "
                  "AND name='libconsumer') AND target_name='libunknown';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "unsupported_visibility");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.resolution'),"
                  "json_extract(properties,'$.visibility_rule') FROM module_dependencies "
                  "WHERE source_id=(SELECT module_id FROM modules WHERE workspace_id=?1 "
                  "AND name='libconsumer') AND target_name='//vendor/two:libqualified';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "explicit_namespace");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "//visibility:public");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.failure_reason') FROM module_dependencies "
                  "WHERE source_id=(SELECT module_id FROM modules WHERE workspace_id=?1 "
                  "AND name='libsubconsumer') AND target_name='libpackage';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "visibility_blocked");
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"libconsumer\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"namespaces\":3"));
    ASSERT_NOT_NULL(strstr(response, "\"namespace_imports\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"packages\":5"));
    ASSERT_NOT_NULL(strstr(response, "\"dependencies_ambiguous\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"dependencies_visibility_blocked\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"dependencies_unsupported_visibility\":1"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_models_filegroups_genrules_and_output_tags) {
    char *root = NULL;
    ASSERT_EQ(create_generated_build_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.blueprint_files, 1);
    ASSERT_EQ(stats.module_count, 6);
    ASSERT_EQ(stats.dependency_count, 12);
    ASSERT_EQ(stats.resolved_count, 11);
    ASSERT_EQ(stats.unresolved_count, 1);
    ASSERT_EQ(stats.filegroup_count, 2);
    ASSERT_EQ(stats.genrule_count, 1);
    ASSERT_EQ(stats.generated_dependency_count, 4);
    ASSERT_EQ(stats.tool_dependency_count, 2);
    ASSERT_EQ(stats.tagged_dependency_count, 5);
    ASSERT_EQ(stats.source_file_count, 6);
    ASSERT_EQ(stats.generated_output_count, 2);
    ASSERT_EQ(stats.tool_file_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.filegroup_path') FROM modules "
                  "WHERE workspace_id=?1 AND name='common_srcs';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "src");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(properties,'$.generator_command') FROM modules "
                  "WHERE workspace_id=?1 AND name='generated';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "$(location host_tool) $(in) $(out)");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(role||':'||path,',') FROM ("
                  "SELECT f.role,f.path FROM module_files f JOIN modules m ON m.module_id=f.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='generated' ORDER BY f.role,f.path);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "OUTPUT:generated.cpp,OUTPUT:generated.h,SOURCE:schema.json,TOOL_FILE:script.py");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(f.properties,'$.origin'),"
                  "json_extract(f.properties,'$.inherited_from') "
                  "FROM module_files f JOIN modules m ON m.module_id=f.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='consumer' "
                  "AND f.path='inherited.cpp' AND f.role='SOURCE';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "defaults");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "b5_defaults");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT d.target_name,d.type,d.resolved,"
                  "json_extract(d.properties,'$.output_tags[0]'),"
                  "json_extract(d.properties,'$.declared_references[0]') "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='common_srcs';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "generated");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "FILEGROUP_INPUT");
    ASSERT_TRUE(sqlite3_column_int(stmt, 2));
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), ".cpp");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4), ":generated{.cpp}");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT d.target_name,d.resolved,json_extract(d.properties,'$.failure_reason'),"
                  "json_extract(d.properties,'$.output_tags[0]') "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='consumer' "
                  "AND d.target_name='missing_generator';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "missing_generator");
    ASSERT_FALSE(sqlite3_column_int(stmt, 1));
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "not_found");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), ".src");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_array_length(d.properties,'$.declared_references'),"
                  "json_extract(d.properties,'$.output_tags[0]') "
                  "FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='consumer' "
                  "AND d.target_name='generated' AND d.type='GENERATED_SOURCE';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), ".cpp");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT count(*) FROM module_dependencies d JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND d.target_name='script.py';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"consumer\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"filegroups\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"genrules\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"generated_dependencies\":4"));
    ASSERT_NOT_NULL(strstr(response, "\"tool_dependencies\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"tagged_dependencies\":5"));
    ASSERT_NOT_NULL(strstr(response, "\"declared_source_files\":6"));
    ASSERT_NOT_NULL(strstr(response, "\"declared_output_files\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"declared_tool_files\":1"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_evaluates_common_android_make_semantics) {
    char *root = NULL;
    ASSERT_EQ(create_make_semantics_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.blueprint_files, 1);
    ASSERT_EQ(stats.make_files, 1);
    ASSERT_EQ(stats.module_count, 5);
    ASSERT_EQ(stats.dependency_count, 4);
    ASSERT_EQ(stats.resolved_count, 4);
    ASSERT_EQ(stats.unresolved_count, 0);
    ASSERT_EQ(stats.source_file_count, 3);
    ASSERT_EQ(stats.make_include_count, 1);
    ASSERT_EQ(stats.make_condition_count, 3);
    ASSERT_EQ(stats.make_macro_count, 2);
    ASSERT_EQ(stats.make_unsupported_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT module_type,json_extract(properties,'$.make_build_rule'),"
                  "json_extract(properties,'$.make_module_class') FROM modules "
                  "WHERE workspace_id=?1 AND name='libmake_audio';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "cc_library_shared");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "BUILD_SHARED_LIBRARY");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "SHARED_LIBRARIES");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT module_type,json_extract(properties,'$.make_module_class') "
                  "FROM modules WHERE workspace_id=?1 AND name='make_app';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "android_app_import");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "APPS");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(target_name||':'||type,',') FROM ("
                  "SELECT d.target_name,d.type FROM module_dependencies d "
                  "JOIN modules m ON m.module_id=d.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='libmake_audio' "
                  "ORDER BY d.target_name,d.type);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "libbase:SHARED_LIB,libcodec_vendor:STATIC_LIB,libutils:SHARED_LIB");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(path,',') FROM ("
                  "SELECT f.path FROM module_files f JOIN modules m ON m.module_id=f.source_id "
                  "WHERE m.workspace_id=?1 AND m.name='libmake_audio' ORDER BY f.path);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "audio.cpp,generated.cpp");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(includes,'$[0]'),condition_count,macro_count,"
                  "json_array_length(unsupported_expressions),"
                  "json_extract(unsupported_expressions,'$[0]') "
                  "FROM build_make_files WHERE workspace_id=?1 AND file_path='Android.mk';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "common.mk");
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 3);
    ASSERT_EQ(sqlite3_column_int(stmt, 2), 2);
    ASSERT_EQ(sqlite3_column_int(stmt, 3), 1);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4),
                  "ifeq ($(TARGET_ARCH),arm64)");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT count(*) FROM modules WHERE workspace_id=?1 "
                  "AND name IN('ignored_else','ignored_ifneq','unknown_arch_module');",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"make\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"make_includes\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"make_conditions\":3"));
    ASSERT_NOT_NULL(strstr(response, "\"make_macro_expansions\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"make_unsupported_expressions\":1"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_models_products_board_configs_and_partition_ownership) {
    char *root = NULL;
    ASSERT_EQ(create_product_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.blueprint_files, 2);
    ASSERT_EQ(stats.make_files, 0);
    ASSERT_EQ(stats.product_make_files, 5);
    ASSERT_EQ(stats.board_config_files, 1);
    ASSERT_EQ(stats.product_count, 1);
    ASSERT_EQ(stats.product_fragment_count, 4);
    ASSERT_EQ(stats.product_inheritance_count, 5);
    ASSERT_EQ(stats.product_inheritance_resolved_count, 3);
    ASSERT_EQ(stats.product_inheritance_cycle_count, 2);
    ASSERT_EQ(stats.product_package_count, 5);
    ASSERT_EQ(stats.product_package_resolved_count, 4);
    ASSERT_EQ(stats.product_package_unresolved_count, 1);
    ASSERT_EQ(stats.board_config_count, 1);
    ASSERT_EQ(stats.product_partition_count, 4);
    ASSERT_EQ(stats.make_unsupported_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT device,brand,model,manufacturer,device_owner,vendor_owner,"
                  "json_extract(partitions,'$[0]'),json_extract(properties,'$.inheritance_cycle') "
                  "FROM build_products WHERE workspace_id=?1 AND name='aosp_demo';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "demo");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "Acme");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "Demo Phone");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "Acme Devices");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4), "device/acme/demo");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 5), "");
    ASSERT_NOT_NULL(sqlite3_column_text(stmt, 6));
    ASSERT_EQ(sqlite3_column_int(stmt, 7), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(inherited_path||':'||status,',') FROM ("
                  "SELECT i.inherited_path,i.status FROM build_product_inheritance i "
                  "JOIN build_products p ON p.product_id=i.source_product_id "
                  "WHERE p.workspace_id=?1 AND p.name='aosp_demo' ORDER BY i.inherited_path);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "device/acme/demo/device.mk:resolved,"
                  "vendor/acme/demo/missing.mk:optional_missing");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT group_concat(module_name||':'||partition_name||':'||resolved||':'||"
                  "json_extract(properties,'$.origin'),',') FROM ("
                  "SELECT pp.* FROM build_product_packages pp JOIN build_products p "
                  "ON p.product_id=pp.product_id WHERE p.workspace_id=?1 AND p.name='aosp_demo' "
                  "AND pp.included=1 ORDER BY pp.module_name);",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "BaseApp:unspecified:1:inherit-product,"
                  "DeviceApp:unspecified:1:inherit-product,"
                  "DirectApp:unspecified:1:direct,"
                  "MissingVendor:vendor:0:direct,"
                  "VendorApp:vendor:1:direct");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(pp.properties,'$.inherited_from'),"
                  "json_extract(pp.properties,'$.inheritance_path'),"
                  "json_extract(pp.properties,'$.inheritance_depth') "
                  "FROM build_product_packages pp JOIN build_products p "
                  "ON p.product_id=pp.product_id WHERE p.workspace_id=?1 "
                  "AND p.name='aosp_demo' AND pp.module_name='BaseApp';",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "device/acme/demo/device.mk");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1),
                  "device/acme/demo/device.mk>vendor/acme/demo/vendor.mk");
    ASSERT_EQ(sqlite3_column_int(stmt, 2), 2);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(
                  master,
                  "SELECT json_extract(variables,'$.TARGET_BOARD_PLATFORM'),"
                  "json_extract(variables,'$.BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE'),"
                  "json_array_length(partitions),device_owner,vendor_owner "
                  "FROM build_board_configs WHERE workspace_id=?1;",
                  -1, &stmt, NULL),
              SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "demo");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "ext4");
    ASSERT_EQ(sqlite3_column_int(stmt, 2), 4);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "device/acme/demo");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 4), "");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
                  "SELECT count(*) FROM schema_versions WHERE version=11;",
                  -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"App\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"products\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"product_fragments\":4"));
    ASSERT_NOT_NULL(strstr(response, "\"product_inheritance\":5"));
    ASSERT_NOT_NULL(strstr(response, "\"product_packages\":5"));
    ASSERT_NOT_NULL(strstr(response, "\"product_packages_resolved\":4"));
    ASSERT_NOT_NULL(strstr(response, "\"board_configs\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"product_partitions\":4"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_graph_imports_bazel_mixed_build_metadata) {
    char *root = NULL;
    ASSERT_EQ(create_bazel_mixed_build_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.bazel_metadata_files, 2);
    ASSERT_EQ(stats.bazel_artifact_count, 2);
    ASSERT_EQ(stats.bazel_target_count, 7);
    ASSERT_EQ(stats.bazel_target_resolved_count, 5);
    ASSERT_EQ(stats.bazel_dependency_count, 7);
    ASSERT_EQ(stats.bazel_dependency_resolved_count, 2);
    ASSERT_EQ(stats.bazel_ambiguous_count, 3);
    ASSERT_EQ(stats.bazel_missing_count, 3);
    ASSERT_EQ(stats.bazel_coverage_gap_count, 10);
    ASSERT_EQ(stats.dependency_count, 7);
    ASSERT_EQ(stats.resolved_count, 2);
    ASSERT_EQ(stats.unresolved_count, 5);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(target_label||':'||status,',') FROM ("
        "SELECT target_label,status FROM build_bazel_dependencies WHERE workspace_id=?1 "
        "ORDER BY target_label);", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "//app:duplicate:target_module_ambiguous,"
                  "//app:local:resolved,"
                  "//app:unmapped:target_module_not_found,"
                  "//missing:target:label_not_found,"
                  "//vendor/lib:shared:resolved,"
                  "//vendor/lib:variant:label_ambiguous,"
                  "@rules_cc//cc:toolchain:external_repository");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT sr.path,tr.path,d.dependency_type,d.transition "
        "FROM build_bazel_dependencies d "
        "JOIN modules sm ON sm.module_id=d.source_module_id "
        "JOIN modules tm ON tm.module_id=d.target_module_id "
        "JOIN repos sr ON sr.repo_id=sm.repo_id JOIN repos tr ON tr.repo_id=tm.repo_id "
        "WHERE d.workspace_id=?1 AND d.target_label='//vendor/lib:shared';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "app");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "vendor/lib");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "BAZEL_LINK");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "target");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM module_edges e JOIN modules m ON m.module_id=e.source_id "
        "WHERE m.workspace_id=?1 AND e.type IN('BAZEL_LINK','BAZEL_DEPENDENCY');",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT json_extract(coverage_gaps,'$[0]') FROM build_bazel_artifacts "
        "WHERE workspace_id=?1 AND repo_id=(SELECT repo_id FROM repos WHERE path='app');",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "select_provider:FeatureFlagInfo");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM schema_versions WHERE version=12;", -1, &stmt, NULL),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.bazel_target_count, 7);
    ASSERT_EQ(stats.bazel_dependency_count, 7);

    ASSERT_EQ(make_dir(root, "app/bad") , 0);
    ASSERT_EQ(write_relative(root, "app/bad/aosp_bazel_mixed_build.json",
                             "{\"schema\":\"wrong\",\"version\":1,\"targets\":[]}"), 0);
    err[0] = '\0';
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), -1);
    ASSERT_NOT_NULL(strstr(err, "cannot parse AOSP build file"));
    char bad_path[4096];
    (void)snprintf(bad_path, sizeof(bad_path),
                   "%s/app/bad/aosp_bazel_mixed_build.json", root);
    ASSERT_EQ(remove(bad_path), 0);
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.bazel_dependency_count, 7);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"app_bin\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"bazel_artifacts\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"bazel_targets\":7"));
    ASSERT_NOT_NULL(strstr(response, "\"bazel_dependencies_resolved\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"bazel_coverage_gaps\":10"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int catalog_build_file_link_repo(const cbm_aosp_workspace_t *workspace,
                                        const cbm_aosp_repo_t *repo,
                                        const char *db_path, bool vendor) {
    sqlite3 *shard = NULL;
    if (sqlite3_open(db_path, &shard) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);";
    const char *app_nodes =
        "INSERT INTO nodes VALUES"
        "(1,'Android.bp','app.Android.bp','File','Android.bp',1,10,'{}'),"
        "(2,'linked_files','build.linked_files','Module','Android.bp',1,1,'{}'),"
        "(3,'local_gen','build.local_gen','Module','Android.bp',2,2,'{}'),"
        "(4,'linked_consumer','build.linked_consumer','Module','Android.bp',3,9,'{}'),"
        "(5,'main.cpp','app.main.cpp','File','main.cpp',1,1,'{}'),"
        "(6,'linked_main','app.linked_main','Function','main.cpp',1,1,'{}'),"
        "(7,'a.cpp','app.src.a.cpp','File','src/a.cpp',1,1,'{}'),"
        "(8,'linked_a','app.linked_a','Function','src/a.cpp',1,1,'{}'),"
        "(9,'ambiguous.py','app.ambiguous.one','File','ambiguous.py',1,1,'{}'),"
        "(10,'ambiguous.py','app.ambiguous.two','File','ambiguous.py',1,1,'{}');";
    const char *vendor_nodes =
        "INSERT INTO nodes VALUES"
        "(1,'Android.bp','vendor.Android.bp','File','Android.bp',1,1,'{}'),"
        "(2,'vendor_gen','build.vendor_gen','Module','Android.bp',1,1,'{}');";
    int rc = sqlite3_exec(shard, schema, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(shard, vendor ? vendor_nodes : app_nodes,
                          NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_close(shard);
    if (rc != 0) return rc;
    char err[512] = {0};
    if (cbm_aosp_catalog_repo_db(workspace, repo, db_path, err, sizeof(err)) != 0) return -1;
    return mark_repo_indexed(workspace, repo->repo_id, db_path);
}

TEST(aosp_build_graph_links_files_generated_outputs_and_definition_symbols) {
    char *root = NULL;
    ASSERT_EQ(create_build_file_links_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    int cataloged = 0;
    for (int i = 0; i < workspace.repo_count; i++) {
        bool is_app = strcmp(workspace.repos[i].path, "app") == 0;
        bool is_vendor = strcmp(workspace.repos[i].path, "vendor") == 0;
        if (!is_app && !is_vendor) continue;
        char shard_path[4096];
        (void)snprintf(shard_path, sizeof(shard_path),
                       "%s/b9-shard-%d.db", root, cataloged++);
        ASSERT_EQ(catalog_build_file_link_repo(&workspace, &workspace.repos[i],
                                               shard_path, is_vendor), 0);
    }
    ASSERT_EQ(cataloged, 2);

    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.module_count, 6);
    ASSERT_EQ(stats.file_link_count, 16);
    ASSERT_EQ(stats.file_link_resolved_count, 10);
    ASSERT_EQ(stats.file_link_unresolved_count, 6);
    ASSERT_EQ(stats.file_link_ambiguous_count, 1);
    ASSERT_EQ(stats.file_link_missing_count, 3);
    ASSERT_EQ(stats.file_link_unindexed_count, 2);
    ASSERT_EQ(stats.generated_file_count, 3);
    ASSERT_EQ(stats.generated_link_count, 3);
    ASSERT_EQ(stats.generated_link_resolved_count, 2);
    ASSERT_EQ(stats.generated_link_unresolved_count, 1);
    ASSERT_EQ(stats.definition_symbol_link_count, 6);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT workspace_path,status,json_extract(properties,'$.definition_symbol_count') "
        "FROM module_file_links WHERE source_id=(SELECT module_id FROM modules "
        "WHERE workspace_id=?1 AND name='linked_files') AND role='SOURCE';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "app/src/a.cpp");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "resolved");
    ASSERT_EQ(sqlite3_column_int(stmt, 2), 1);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT workspace_path,status FROM module_file_links WHERE source_id="
        "(SELECT module_id FROM modules WHERE workspace_id=?1 "
        "AND name='invalid_path_group') AND role='SOURCE';", -1, &stmt, NULL),
        SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "invalid_path");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(declared_path||':'||role||':'||status,',') FROM ("
        "SELECT declared_path,role,status FROM module_file_links WHERE source_id="
        "(SELECT module_id FROM modules WHERE workspace_id=?1 AND name='linked_consumer') "
        "ORDER BY role,declared_path);", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "Android.bp:DEFINITION:resolved,"
                  "../../escape.cpp:SOURCE:invalid_path,main.cpp:SOURCE:resolved,"
                  "missing.cpp:SOURCE:file_not_found,"
                  "ambiguous.py:TOOL_FILE:ambiguous");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(output_tag||':'||status,',') FROM ("
        "SELECT output_tag,status FROM module_generated_links WHERE source_id="
        "(SELECT module_id FROM modules WHERE workspace_id=?1 AND name='linked_consumer') "
        "ORDER BY output_tag);", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  ":resolved,.cpp:resolved,.header:output_not_found");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT r.path,g.workspace_path FROM module_generated_links l "
        "JOIN build_generated_files g ON g.generated_id=l.generated_id "
        "JOIN repos r ON r.repo_id=g.repo_id WHERE l.source_id="
        "(SELECT module_id FROM modules WHERE workspace_id=?1 AND name='linked_consumer') "
        "AND l.output_tag='';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "vendor");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "vendor/vendor.out");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(link,',') FROM (SELECT s.name||':'||l.link_type AS link "
        "FROM module_symbol_links l JOIN symbols s ON s.global_id=l.symbol_global_id "
        "WHERE l.source_id="
        "(SELECT module_id FROM modules WHERE workspace_id=?1 AND name='linked_consumer') "
        "ORDER BY s.name);", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "linked_consumer:BUILD_DEFINITION,linked_main:SOURCE_DEFINITION");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM schema_versions WHERE version=13;", -1, &stmt, NULL),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.file_link_count, 16);
    ASSERT_EQ(stats.definition_symbol_link_count, 6);

    char args[8192];
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"linked\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"file_links\":16"));
    ASSERT_NOT_NULL(strstr(response, "\"file_links_resolved\":10"));
    ASSERT_NOT_NULL(strstr(response, "\"generated_files\":3"));
    ASSERT_NOT_NULL(strstr(response, "\"generated_file_links_resolved\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"definition_symbol_links\":6"));
    free(response);

    ASSERT_EQ(write_relative(root, "unindexed/Android.bp", "// module removed\n"), 0);
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.module_count, 5);
    ASSERT_EQ(stats.file_link_count, 14);
    ASSERT_EQ(stats.file_link_unindexed_count, 0);
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM module_file_links l "
        "LEFT JOIN modules m ON m.module_id=l.source_id WHERE m.module_id IS NULL;",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_build_queries_expose_edges_variants_gaps_and_provenance) {
    char *root = NULL;
    ASSERT_EQ(create_variants_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_build_stats_t stats;
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);

    cbm_aosp_module_t *modules = NULL;
    int count = 0;
    bool truncated = false;
    ASSERT_EQ(cbm_aosp_search_modules(&workspace, "libvariant_consumer", 10,
                                      &modules, &count, err, sizeof(err)), 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(cbm_aosp_load_module_details(&workspace, modules, count, 100,
                                           &truncated, err, sizeof(err)), 0);
    ASSERT_FALSE(truncated);
    ASSERT_NOT_NULL(strstr(modules[0].properties, "\"compile_multilib\":\"both\""));
    ASSERT_TRUE(modules[0].dependency_count >= 13);
    bool found_inherited_variant = false;
    for (int i = 0; i < modules[0].dependency_count; i++) {
        cbm_aosp_module_dependency_t *dependency = &modules[0].dependencies[i];
        if (strcmp(dependency->target_name, "libfromdefaults") != 0) continue;
        ASSERT_TRUE(dependency->resolved);
        ASSERT_STR_EQ(dependency->target_repo_path, "project");
        ASSERT_NOT_NULL(strstr(dependency->properties, "\"origin\":\"defaults\""));
        ASSERT_NOT_NULL(strstr(dependency->properties,
                               "target.android&arch.arm64"));
        found_inherited_variant = true;
    }
    ASSERT_TRUE(found_inherited_variant);
    cbm_aosp_modules_free(modules, count);

    modules = NULL;
    count = 0;
    truncated = false;
    ASSERT_EQ(cbm_aosp_search_modules(&workspace, "libvariant_consumer", 10,
                                      &modules, &count, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_load_module_details(&workspace, modules, count, 1,
                                           &truncated, err, sizeof(err)), 0);
    ASSERT_TRUE(truncated);
    ASSERT_EQ(modules[0].dependency_count, 1);
    ASSERT_TRUE(modules[0].details_truncated);
    cbm_aosp_modules_free(modules, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"libvariant_consumer\","
        "\"detail_limit\":100}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"details_included\":true"));
    ASSERT_NOT_NULL(strstr(response, "\"dependencies\""));
    ASSERT_NOT_NULL(strstr(response, "target.android&arch.arm64"));
    ASSERT_NOT_NULL(strstr(response, "\"origin\":\"defaults\""));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);

    root = NULL;
    ASSERT_EQ(create_make_semantics_workspace_fixture(&root), 0);
    memset(&workspace, 0, sizeof(workspace));
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_build_scan(&workspace, &stats, err, sizeof(err)), 0);
    cbm_aosp_build_gap_t *gaps = NULL;
    int gap_count = 0;
    truncated = false;
    ASSERT_EQ(cbm_aosp_build_gaps(&workspace, 10, &gaps, &gap_count, &truncated,
                                  err, sizeof(err)), 0);
    ASSERT_FALSE(truncated);
    ASSERT_EQ(gap_count, 1);
    ASSERT_STR_EQ(gaps[0].kind, "make_expression");
    ASSERT_STR_EQ(gaps[0].repo_path, "project");
    ASSERT_STR_EQ(gaps[0].subject, "ifeq ($(TARGET_ARCH),arm64)");
    ASSERT_STR_EQ(gaps[0].status, "unsupported_expression");
    cbm_aosp_build_gaps_free(gaps, gap_count);

    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"query\":\"make\"}", root);
    response = cbm_mcp_handle_tool(NULL, "aosp_get_architecture", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"coverage_gaps\""));
    ASSERT_NOT_NULL(strstr(response, "ifeq ($(TARGET_ARCH),arm64)"));
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_protocol_graph_models_complete_aidl_declarations) {
    char *root = NULL;
    ASSERT_EQ(create_complete_aidl_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    cbm_aosp_protocol_stats_t stats;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 2);
    ASSERT_EQ(stats.aidl_methods, 3);
    ASSERT_EQ(stats.aidl_parcelables, 1);
    ASSERT_EQ(stats.aidl_unions, 1);
    ASSERT_EQ(stats.aidl_enums, 1);
    ASSERT_EQ(stats.aidl_imports, 3);
    ASSERT_EQ(stats.aidl_callbacks, 1);
    ASSERT_EQ(stats.aidl_oneway_methods, 2);
    ASSERT_EQ(stats.aidl_stable_types, 3);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(item,',') FROM (SELECT name||':'||"
        "json_extract(properties,'$.resolution') AS item FROM protocol_nodes "
        "WHERE workspace_id=?1 AND kind='AIDL_IMPORT' ORDER BY name);",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "android.test.Missing:not_found,android.test.Result:resolved,"
                  "com.acme.ICallback:resolved");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(item,',') FROM (SELECT t.name||':'||"
        "json_extract(e.properties,'$.resolution') AS item FROM protocol_edges e "
        "JOIN protocol_nodes t ON t.protocol_id=e.target_id "
        "WHERE t.workspace_id=?1 AND e.type='DECLARES_IMPORT' ORDER BY t.name);",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "android.test.Missing:not_found,android.test.Result:resolved,"
                  "com.acme.ICallback:resolved");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_edges e JOIN protocol_nodes s "
        "ON s.protocol_id=e.source_id WHERE s.workspace_id=?1 "
        "AND s.kind='AIDL_IMPORT' AND e.type='RESOLVES_TO';", -1, &stmt, NULL),
        SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT json_extract(properties,'$.oneway'),"
        "json_extract(properties,'$.annotations[0]') FROM protocol_nodes "
        "WHERE workspace_id=?1 AND kind='AIDL_METHOD' AND name='registerCallback';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_TRUE(sqlite3_column_int(stmt, 0));
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "EnforcePermission");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT s.qualified_name||'->'||t.qualified_name FROM protocol_edges e "
        "JOIN protocol_nodes s ON s.protocol_id=e.source_id "
        "JOIN protocol_nodes t ON t.protocol_id=e.target_id "
        "WHERE s.workspace_id=?1 AND e.type='USES_CALLBACK';", -1, &stmt, NULL),
        SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "android.test.IService.registerCallback->com.acme.ICallback");
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND kind='AIDL_FIELD';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 6);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 "
        "AND kind='AIDL_FIELD' AND name='mode';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT json_extract(properties,'$.annotations[0]') FROM protocol_nodes "
        "WHERE workspace_id=?1 AND kind='AIDL_FIELD' AND name='tagged';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "FieldTag");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 "
        "AND kind='AIDL_ENUM_VALUE';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    cbm_aosp_protocol_node_t *nodes = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_protocols(&workspace, "IService", 50, &nodes, &count,
                                        err, sizeof(err)), 0);
    ASSERT_TRUE(count >= 3);
    bool found_stability = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(nodes[i].kind, "AIDL_INTERFACE") == 0 &&
            strstr(nodes[i].properties, "\"stability\":\"vintf\"")) {
            found_stability = true;
        }
    }
    ASSERT_TRUE(found_stability);
    cbm_aosp_protocol_nodes_free(nodes, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"IService\",\"limit\":50}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_trace_protocol", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"aidl_parcelables\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"aidl_callbacks\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"aidl_oneway_methods\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"stability\":\"vintf\""));
    ASSERT_NOT_NULL(strstr(response, "USES_CALLBACK"));
    free(response);

    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 2);
    ASSERT_EQ(stats.aidl_imports, 3);
    ASSERT_EQ(stats.aidl_callbacks, 1);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_protocol_graph_links_binder_transaction_flow) {
    char *root = NULL;
    ASSERT_EQ(create_binder_flow_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_path[4096];
    (void)snprintf(shard_path, sizeof(shard_path), "%s/binder-flow-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(2,'start','android.media.BpAudioService.start','Method',"
        "'android/media/AudioBinder.cpp',14,16,'{}'),"
        "(3,'start','android.media.AudioService.start','Method',"
        "'android/media/AudioBinder.cpp',3,3,'{}'),"
        "(5,'onTransact','android.media.BnAudioService.onTransact','Method',"
        "'android/media/AudioBinder.cpp',9,13,'{}'),"
        "(6,'start','android.media.Unrelated.start','Method',"
        "'android/media/AudioBinder.cpp',20,20,'{}'),"
        "(8,'stop','android.media.BpAudioService.stop','Method',"
        "'android/media/AudioBinder.cpp',17,19,'{}'),"
        "(9,'stop','android.media.AudioService.stop','Method',"
        "'android/media/AudioBinder.cpp',4,4,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], shard_path,
                                       err, sizeof(err)), 0);

    cbm_aosp_protocol_stats_t stats;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 1);
    ASSERT_EQ(stats.aidl_methods, 2);
    ASSERT_EQ(stats.binder_server_edges, 0);
    ASSERT_EQ(stats.binder_client_edges, 2);
    ASSERT_EQ(stats.binder_transaction_constants, 2);
    ASSERT_EQ(stats.binder_on_transact_handlers, 1);
    ASSERT_EQ(stats.binder_transact_calls, 2);
    ASSERT_EQ(stats.binder_implementation_methods, 2);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(type,',') FROM (SELECT type FROM protocol_edges e "
        "JOIN protocol_nodes s ON s.protocol_id=e.source_id WHERE s.workspace_id=?1 "
        "AND type IN('BINDER_TRANSACTION','BINDER_DISPATCH_CASE','BINDER_DISPATCHES_TO',"
        "'BINDER_TRANSACT_CALL','BINDER_IMPLEMENTED_BY') ORDER BY type);",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
        "BINDER_DISPATCHES_TO,BINDER_DISPATCHES_TO,BINDER_DISPATCH_CASE,"
        "BINDER_DISPATCH_CASE,BINDER_IMPLEMENTED_BY,BINDER_IMPLEMENTED_BY,"
        "BINDER_TRANSACTION,BINDER_TRANSACTION,BINDER_TRANSACT_CALL,"
        "BINDER_TRANSACT_CALL");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 "
        "AND kind='BINDER_IMPLEMENTATION_METHOD' AND qualified_name LIKE '%Unrelated%';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"start\",\"limit\":50}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_trace_protocol", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"binder_transaction_constants\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"binder_on_transact_handlers\":1"));
    ASSERT_NOT_NULL(strstr(response, "\"binder_transact_calls\":2"));
    ASSERT_NOT_NULL(strstr(response, "\"binder_implementation_methods\":2"));
    ASSERT_NOT_NULL(strstr(response, "binder_proxy_transact_call"));
    ASSERT_NOT_NULL(strstr(response, "binder_direct_inheritance"));
    free(response);

    int edge_count = stats.edge_count;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, edge_count);
    ASSERT_EQ(stats.binder_transact_calls, 2);
    ASSERT_EQ(stats.binder_implementation_methods, 2);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_protocol_graph_models_generated_binder_backends) {
    char *root = NULL;
    ASSERT_EQ(create_binder_backends_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_path[4096];
    (void)snprintf(shard_path, sizeof(shard_path), "%s/binder-backends-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(1,'IAudioService','android.media.IAudioService','Interface','generated/IAudioService.java',2,11,'{}'),"
        "(2,'Stub','android.media.IAudioService.Stub','Class','generated/IAudioService.java',3,10,'{}'),"
        "(3,'Proxy','android.media.IAudioService.Stub.Proxy','Class','generated/IAudioService.java',7,9,'{}'),"
        "(5,'run','android.media.IAudioService.Stub.run','Method','generated/IAudioService.java',5,5,'{}'),"
        "(6,'onTransact','android.media.IAudioService.Stub.onTransact','Method','generated/IAudioService.java',6,6,'{}'),"
        "(7,'run','android.media.IAudioService.Stub.Proxy.run','Method','generated/IAudioService.java',8,8,'{}'),"
        "(8,'run','android.media.JavaService.run','Method','generated/JavaService.java',3,3,'{}'),"
        "(9,'IAudioService','android.media.cpp.IAudioService','Class','generated/IAudioService.cpp',1,7,'{}'),"
        "(10,'BnAudioService','android.media.cpp.BnAudioService','Class','generated/IAudioService.cpp',1,7,'{}'),"
        "(11,'BpAudioService','android.media.cpp.BpAudioService','Class','generated/IAudioService.cpp',1,7,'{}'),"
        "(12,'TRANSACTION_run','android.media.cpp.BnAudioService.TRANSACTION_run','Constant','generated/IAudioService.cpp',4,4,'{}'),"
        "(13,'run','android.media.cpp.BnAudioService.run','Method','generated/IAudioService.cpp',5,5,'{}'),"
        "(14,'onTransact','android.media.cpp.BnAudioService.onTransact','Method','generated/IAudioService.cpp',6,6,'{}'),"
        "(15,'run','android.media.cpp.BpAudioService.run','Method','generated/IAudioService.cpp',7,7,'{}'),"
        "(16,'run','android.media.cpp.CppService.run','Method','generated/IAudioService.cpp',2,2,'{}'),"
        "(17,'IAudioService','android.media.rust.IAudioService','Trait','generated/IAudioService.rs',1,14,'{}'),"
        "(18,'BnAudioService','android.media.rust.BnAudioService','Struct','generated/IAudioService.rs',6,10,'{}'),"
        "(19,'BpAudioService','android.media.rust.BpAudioService','Struct','generated/IAudioService.rs',11,14,'{}'),"
        "(21,'run','android.media.rust.BnAudioService.run','Method','generated/IAudioService.rs',8,8,'{}'),"
        "(22,'on_transact','android.media.rust.BnAudioService.on_transact','Method','generated/IAudioService.rs',9,9,'{}'),"
        "(23,'run','android.media.rust.BpAudioService.run','Method','generated/IAudioService.rs',13,13,'{}'),"
        "(24,'run','android.media.rust.RustService.run','Method','generated/IAudioService.rs',3,3,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], shard_path,
                                       err, sizeof(err)), 0);

    cbm_aosp_protocol_stats_t stats;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 1);
    ASSERT_EQ(stats.aidl_methods, 1);
    ASSERT_EQ(stats.aidl_java_generated_nodes, 7);
    ASSERT_EQ(stats.aidl_cpp_ndk_generated_nodes, 7);
    ASSERT_EQ(stats.aidl_rust_generated_nodes, 7);
    ASSERT_EQ(stats.binder_server_edges, 3);
    ASSERT_EQ(stats.binder_client_edges, 3);
    ASSERT_EQ(stats.binder_transaction_constants, 3);
    ASSERT_EQ(stats.binder_on_transact_handlers, 3);
    ASSERT_EQ(stats.binder_transact_calls, 3);
    ASSERT_EQ(stats.binder_implementation_methods, 3);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 AND "
        "((kind='BINDER_SERVER_TYPE' AND name='Stub') OR "
        "(kind='BINDER_CLIENT_TYPE' AND name='Proxy'));", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_edges e "
        "JOIN protocol_nodes s ON s.protocol_id=e.source_id "
        "JOIN protocol_nodes t ON t.protocol_id=e.target_id "
        "WHERE s.workspace_id=?1 AND e.type IN('BINDER_DISPATCH_CASE',"
        "'BINDER_DISPATCHES_TO','BINDER_TRANSACT_CALL','BINDER_IMPLEMENTED_BY') "
        "AND CASE WHEN lower(s.file_path) LIKE '%.java' OR lower(s.file_path) LIKE '%.kt' "
        "THEN 'java' WHEN lower(s.file_path) LIKE '%.rs' THEN 'rust' ELSE 'cpp_ndk' END "
        "<> CASE WHEN lower(t.file_path) LIKE '%.java' OR lower(t.file_path) LIKE '%.kt' "
        "THEN 'java' WHEN lower(t.file_path) LIKE '%.rs' THEN 'rust' ELSE 'cpp_ndk' END;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_edges e JOIN protocol_nodes s "
        "ON s.protocol_id=e.source_id WHERE s.workspace_id=?1 AND "
        "json_extract(e.properties,'$.transaction')='transactions::run';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 5);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"run\",\"limit\":100}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_trace_protocol", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"aidl_java_generated_nodes\":7"));
    ASSERT_NOT_NULL(strstr(response, "\"aidl_cpp_ndk_generated_nodes\":7"));
    ASSERT_NOT_NULL(strstr(response, "\"aidl_rust_generated_nodes\":7"));
    ASSERT_NOT_NULL(strstr(response, "transactions::run"));
    free(response);

    int edge_count = stats.edge_count;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, edge_count);
    ASSERT_EQ(stats.aidl_java_generated_nodes, 7);
    ASSERT_EQ(stats.aidl_cpp_ndk_generated_nodes, 7);
    ASSERT_EQ(stats.aidl_rust_generated_nodes, 7);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_protocol_graph_links_service_manager_paths) {
    char *root = NULL;
    ASSERT_EQ(create_service_manager_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    const cbm_aosp_repo_t *server_repo = NULL;
    const cbm_aosp_repo_t *client_repo = NULL;
    for (int i = 0; i < workspace.repo_count; i++) {
        if (strcmp(workspace.repos[i].path, "server") == 0) server_repo = &workspace.repos[i];
        if (strcmp(workspace.repos[i].path, "client") == 0) client_repo = &workspace.repos[i];
    }
    ASSERT_NOT_NULL(server_repo);
    ASSERT_NOT_NULL(client_repo);

    char server_shard_path[4096];
    char client_shard_path[4096];
    (void)snprintf(server_shard_path, sizeof(server_shard_path), "%s/server-shard.db", root);
    (void)snprintf(client_shard_path, sizeof(client_shard_path), "%s/client-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(server_shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(1,'start','android.media.AudioService.start','Method','AudioServer.cpp',4,4,'{}'),"
        "(2,'publishAudio','android.media.publishAudio','Function','AudioServer.cpp',6,8,'{}'),"
        "(3,'publishDynamic','android.media.publishDynamic','Function',"
        "'AudioServer.cpp',9,11,'{}'),"
        "(4,'publishNdk','android.media.publishNdk','Function','AudioServer.cpp',12,14,'{}'),"
        "(5,'start','android.media.OtherAudioService.start','Method','AudioServer.cpp',17,17,'{}'),"
        "(6,'publishAmbiguousOne','android.media.publishAmbiguousOne','Function',"
        "'AudioServer.cpp',19,21,'{}'),"
        "(7,'publishAmbiguousTwo','android.media.publishAmbiguousTwo','Function',"
        "'AudioServer.cpp',22,24,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, server_repo, server_shard_path,
                                       err, sizeof(err)), 0);

    ASSERT_EQ(sqlite3_open(client_shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(1,'connectAudio','android.media.connectAudio','Function','AudioClient.cpp',1,4,'{}'),"
        "(2,'waitAudio','android.media.waitAudio','Function','AudioClient.cpp',5,8,'{}'),"
        "(3,'checkMissing','android.media.checkMissing','Function','AudioClient.cpp',9,11,'{}'),"
        "(4,'connectJava','android.media.JavaClient.connectJava','Method',"
        "'JavaClient.java',2,4,'{}'),"
        "(5,'connectAmbiguous','android.media.JavaClient.connectAmbiguous','Method',"
        "'JavaClient.java',5,7,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, client_repo, client_shard_path,
                                       err, sizeof(err)), 0);

    cbm_aosp_protocol_stats_t stats;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 3);
    ASSERT_EQ(stats.binder_implementation_methods, 2);
    ASSERT_EQ(stats.binder_services, 5);
    ASSERT_EQ(stats.binder_service_registrations, 4);
    ASSERT_EQ(stats.binder_service_lookups, 4);
    ASSERT_EQ(stats.binder_service_waits, 1);
    ASSERT_EQ(stats.binder_service_server_links, 1);
    ASSERT_EQ(stats.binder_service_interface_links, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *master = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &master), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT group_concat(item,',') FROM (SELECT name||':'||"
        "json_extract(properties,'$.server_resolution')||':'||"
        "json_extract(properties,'$.interface_resolution') AS item "
        "FROM protocol_nodes WHERE workspace_id=?1 AND kind='BINDER_SERVICE' ORDER BY name);",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "ambiguous.audio:not_found:ambiguous,"
                  "ambiguous.server:ambiguous:not_found,media.audio:resolved:resolved,"
                  "missing.audio:not_found:not_found,"
                  "ndk.audio:not_found:not_found");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_nodes WHERE workspace_id=?1 "
        "AND kind LIKE 'BINDER_SERVICE_%' AND properties LIKE '%publishDynamic%';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_edges e JOIN protocol_nodes s "
        "ON s.protocol_id=e.source_id WHERE s.workspace_id=?1 "
        "AND e.type='CALLS_SERVICE_MANAGER';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 9);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_edges e JOIN protocol_nodes s "
        "ON s.protocol_id=e.source_id WHERE s.workspace_id=?1 "
        "AND s.kind='BINDER_SERVICE' AND s.name='ambiguous.audio' "
        "AND e.type='BINDER_SERVICE_INTERFACE';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(master,
        "SELECT count(*) FROM protocol_edges e JOIN protocol_nodes s "
        "ON s.protocol_id=e.source_id WHERE s.workspace_id=?1 "
        "AND s.kind='BINDER_SERVICE' AND s.name='ambiguous.server' "
        "AND e.type='BINDER_SERVICE_SERVER';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(master);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"media.audio\",\"limit\":50}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_trace_protocol", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"binder_services\":5"));
    ASSERT_NOT_NULL(strstr(response, "\"binder_service_registrations\":4"));
    ASSERT_NOT_NULL(strstr(response, "\"binder_service_lookups\":4"));
    ASSERT_NOT_NULL(strstr(response, "\"binder_service_waits\":1"));
    ASSERT_NOT_NULL(strstr(response, "service_registration_implementation"));
    ASSERT_NOT_NULL(strstr(response, "service_client_interface"));
    free(response);

    int edge_count = stats.edge_count;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, edge_count);
    ASSERT_EQ(stats.binder_services, 5);
    ASSERT_EQ(stats.binder_service_server_links, 1);
    ASSERT_EQ(stats.binder_service_interface_links, 1);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_protocol_graph_links_binder_and_jni_evidence) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_path[4096];
    (void)snprintf(shard_path, sizeof(shard_path), "%s/protocol-shard.db", root);
    sqlite3 *shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_path, &shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(shard,
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);"
        "INSERT INTO nodes VALUES"
        "(1,'start','aosp.media.BnAudioService.start','Method','media/BnAudioService.cpp',1,3,'{}'),"
        "(2,'start','aosp.media.BpAudioService.start','Method','media/BpAudioService.cpp',1,3,'{}'),"
        "(3,'nativeOpen','aosp.android.media.AudioSystem.nativeOpen','Method','media/AudioSystem.java',4,4,'{}'),"
        "(4,'Java_android_media_AudioSystem_nativeOpen','aosp.jni.Java_android_media_AudioSystem_nativeOpen','Function','media/jni.cpp',5,5,'{}'),"
        "(5,'nativeClose','aosp.android.media.AudioSystem.nativeClose','Method','media/AudioSystem.java',6,6,'{}'),"
        "(6,'nativeClose','aosp.jni.nativeClose','Function','media/jni.cpp',1,1,'{}'),"
        "(7,'nativeClose','aosp.other.nativeClose','Function','media/other.cpp',1,1,'{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(shard);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], shard_path,
                                       err, sizeof(err)), 0);

    cbm_aosp_protocol_stats_t stats;
    ASSERT_EQ(cbm_aosp_protocol_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.aidl_interfaces, 1);
    ASSERT_EQ(stats.aidl_methods, 1);
    ASSERT_EQ(stats.binder_server_edges, 1);
    ASSERT_EQ(stats.binder_client_edges, 1);
    ASSERT_EQ(stats.jni_static_edges, 1);
    ASSERT_EQ(stats.jni_dynamic_edges, 1);

    cbm_aosp_protocol_node_t *nodes = NULL;
    int count = 0;
    ASSERT_EQ(cbm_aosp_search_protocols(&workspace, "IAudioService", 20, &nodes,
                                        &count, err, sizeof(err)), 0);
    ASSERT(count >= 2);
    cbm_aosp_protocol_nodes_free(nodes, count);

    char args[8192];
    (void)snprintf(args, sizeof(args),
        "{\"workspace_root\":\"%s\",\"query\":\"IAudioService\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_trace_protocol", args);
    ASSERT(response != NULL);
    ASSERT(strstr(response, "\"isError\":false") != NULL);
    ASSERT(strstr(response, "binder_server_edges") != NULL);
    ASSERT(strstr(response, "aidl_method_generated_owner") != NULL);
    free(response);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int catalog_single_symbol(const cbm_aosp_workspace_t *workspace,
                                 const cbm_aosp_repo_t *repo, const char *db_path,
                                 const char *name, const char *qualified_name) {
    sqlite3 *shard = NULL;
    if (sqlite3_open(db_path, &shard) != SQLITE_OK) return -1;
    const char *schema =
        "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
        "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);";
    if (sqlite3_exec(shard, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(shard);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(shard,
            "INSERT INTO nodes VALUES(1,?1,?2,'Function','source.cpp',1,2,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(shard);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, qualified_name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(shard);
    if (rc != 0) return rc;
    char err[512] = {0};
    return cbm_aosp_catalog_repo_db(workspace, repo, db_path, err, sizeof(err));
}

static char *find_symbol_id(const cbm_aosp_workspace_t *workspace, const char *name) {
    cbm_aosp_symbol_t *symbols = NULL;
    int count = 0;
    char err[512] = {0};
    if (cbm_aosp_search_symbols(workspace, name, 10, &symbols, &count,
                                err, sizeof(err)) != 0 || count != 1) {
        cbm_aosp_symbols_free(symbols, count);
        return NULL;
    }
    char master_path[4096];
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char *global_id = NULL;
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) == 0 &&
        sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db,
            "SELECT global_id FROM symbols WHERE workspace_id=?1 AND name=?2;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, workspace->workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *value = (const char *)sqlite3_column_text(stmt, 0);
            global_id = value ? strdup(value) : NULL;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    cbm_aosp_symbols_free(symbols, count);
    return global_id;
}

TEST(aosp_cross_edges_are_deterministic_and_refresh_per_source_repo) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char source_db[4096];
    char target_db[4096];
    (void)snprintf(source_db, sizeof(source_db), "%s/source-shard.db", root);
    (void)snprintf(target_db, sizeof(target_db), "%s/target-shard.db", root);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[0], source_db,
                                    "CallVendor", "aosp.framework.CallVendor"), 0);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[1], target_db,
                                    "VendorEntry", "aosp.vendor.VendorEntry"), 0);
    char *source_id = find_symbol_id(&workspace, "CallVendor");
    char *target_id = find_symbol_id(&workspace, "VendorEntry");
    ASSERT_NOT_NULL(source_id);
    ASSERT_NOT_NULL(target_id);

    cbm_aosp_cross_edge_candidate_t candidates[] = {
        {.source_global_id = source_id, .target_global_id = target_id,
         .target_name = "VendorEntry", .type = "CALLS",
         .status = CBM_AOSP_CROSS_EDGE_RESOLVED, .confidence = 0.95,
         .evidence = "qualified_call", .properties = "{}"},
        {.source_global_id = source_id, .target_global_id = target_id,
         .target_name = "VendorEntry", .type = "IMPLEMENTS",
         .status = CBM_AOSP_CROSS_EDGE_AMBIGUOUS, .confidence = 0.5,
         .evidence = "multiple_type_candidates", .properties = "{}"},
        {.source_global_id = source_id, .target_name = "MissingType", .type = "USES_TYPE",
         .status = CBM_AOSP_CROSS_EDGE_UNRESOLVED, .confidence = 0.0,
         .evidence = "unresolved_type", .properties = "{}"},
    };
    cbm_aosp_cross_edge_stats_t stats;
    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&workspace, &workspace.repos[0], candidates, 3,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 3);
    ASSERT_EQ(stats.resolved_count, 1);
    ASSERT_EQ(stats.ambiguous_count, 1);
    ASSERT_EQ(stats.unresolved_count, 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT group_concat(edge_id,'|') FROM "
        "(SELECT edge_id FROM cross_symbol_edges ORDER BY edge_id);", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *first_ids_text = (const char *)sqlite3_column_text(stmt, 0);
    char *first_ids = first_ids_text ? strdup(first_ids_text) : NULL;
    ASSERT_NOT_NULL(first_ids);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&workspace, &workspace.repos[0], candidates, 3,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 3);
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT group_concat(edge_id,'|') FROM "
        "(SELECT edge_id FROM cross_symbol_edges ORDER BY edge_id);", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), first_ids);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&workspace, &workspace.repos[0], candidates, 1,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);
    ASSERT_EQ(stats.resolved_count, 1);
    ASSERT_EQ(stats.unresolved_count, 0);

    free(first_ids);
    free(source_id);
    free(target_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_cross_edges_migrate_v3_schema_without_losing_resolved_edges) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char source_db[4096];
    char target_db[4096];
    (void)snprintf(source_db, sizeof(source_db), "%s/migrate-source.db", root);
    (void)snprintf(target_db, sizeof(target_db), "%s/migrate-target.db", root);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[0], source_db,
                                    "LegacySource", "legacy.Source"), 0);
    ASSERT_EQ(catalog_single_symbol(&workspace, &workspace.repos[1], target_db,
                                    "LegacyTarget", "legacy.Target"), 0);
    char *source_id = find_symbol_id(&workspace, "LegacySource");
    char *target_id = find_symbol_id(&workspace, "LegacyTarget");
    ASSERT_NOT_NULL(source_id);
    ASSERT_NOT_NULL(target_id);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "DROP TABLE cross_symbol_edges;"
        "DELETE FROM schema_versions WHERE version=4;"
        "CREATE TABLE cross_symbol_edges("
        "source_global_id TEXT NOT NULL,target_global_id TEXT NOT NULL,type TEXT NOT NULL,"
        "confidence REAL NOT NULL DEFAULT 0,evidence TEXT DEFAULT '',properties TEXT DEFAULT '{}',"
        "PRIMARY KEY(source_global_id,target_global_id,type));",
        NULL, NULL, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO cross_symbol_edges VALUES(?1,?2,'CALLS',0.8,'legacy_test','{}');",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    cbm_aosp_cross_edge_stats_t stats;
    ASSERT_EQ(cbm_aosp_cross_edge_stats(&workspace, &workspace.repos[0], &stats,
                                        err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);
    ASSERT_EQ(stats.resolved_count, 1);
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT length(edge_id),status,evidence,target_leaf FROM cross_symbol_edges;",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), CBM_AOSP_EDGE_ID_LEN);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "resolved");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "legacy_test");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "LegacyTarget");
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "DROP TABLE cross_symbol_edges;"
        "DELETE FROM schema_versions WHERE version=5;"
        "CREATE TABLE cross_symbol_edges("
        "edge_id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL,source_repo_id TEXT NOT NULL,"
        "target_repo_id TEXT,source_global_id TEXT NOT NULL,target_global_id TEXT,"
        "target_name TEXT NOT NULL DEFAULT '',type TEXT NOT NULL,status TEXT NOT NULL,"
        "confidence REAL NOT NULL DEFAULT 0,evidence TEXT NOT NULL DEFAULT '',"
        "source_generation TEXT NOT NULL DEFAULT '',properties TEXT NOT NULL DEFAULT '{}');",
        NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM pragma_table_info('cross_symbol_edges') WHERE name='target_leaf';",
        -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM schema_versions WHERE version=5;", -1, &stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    free(source_id);
    free(target_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_cross_edges_enforce_workspace_and_repository_boundaries) {
    char *left_root = NULL;
    char *right_root = NULL;
    ASSERT_EQ(create_workspace_fixture(&left_root), 0);
    ASSERT_EQ(create_workspace_fixture(&right_root), 0);
    cbm_aosp_workspace_t left;
    cbm_aosp_workspace_t right;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(left_root, &left, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_discover(right_root, &right, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&left, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&right, err, sizeof(err)), 0);

    char left_source_db[4096];
    char left_target_db[4096];
    char right_source_db[4096];
    char right_target_db[4096];
    (void)snprintf(left_source_db, sizeof(left_source_db), "%s/source.db", left_root);
    (void)snprintf(left_target_db, sizeof(left_target_db), "%s/target.db", left_root);
    (void)snprintf(right_source_db, sizeof(right_source_db), "%s/source.db", right_root);
    (void)snprintf(right_target_db, sizeof(right_target_db), "%s/target.db", right_root);
    ASSERT_EQ(catalog_single_symbol(&left, &left.repos[0], left_source_db,
                                    "CallVendor", "same.CallVendor"), 0);
    ASSERT_EQ(catalog_single_symbol(&left, &left.repos[1], left_target_db,
                                    "VendorEntry", "same.VendorEntry"), 0);
    ASSERT_EQ(catalog_single_symbol(&right, &right.repos[0], right_source_db,
                                    "CallVendor", "same.CallVendor"), 0);
    ASSERT_EQ(catalog_single_symbol(&right, &right.repos[1], right_target_db,
                                    "VendorEntry", "same.VendorEntry"), 0);
    char *left_source = find_symbol_id(&left, "CallVendor");
    char *left_target = find_symbol_id(&left, "VendorEntry");
    char *right_source = find_symbol_id(&right, "CallVendor");
    char *right_target = find_symbol_id(&right, "VendorEntry");
    ASSERT_NOT_NULL(left_source);
    ASSERT_NOT_NULL(left_target);
    ASSERT_NOT_NULL(right_source);
    ASSERT_NOT_NULL(right_target);

    cbm_aosp_cross_edge_candidate_t left_edge = {
        .source_global_id = left_source, .target_global_id = left_target,
        .target_name = "VendorEntry", .type = "CALLS",
        .status = CBM_AOSP_CROSS_EDGE_RESOLVED, .confidence = 1.0,
        .evidence = "exact_qualified_name", .properties = "{}",
    };
    cbm_aosp_cross_edge_candidate_t right_edge = left_edge;
    right_edge.source_global_id = right_source;
    right_edge.target_global_id = right_target;
    cbm_aosp_cross_edge_stats_t stats;
    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&left, &left.repos[0], &left_edge, 1,
                                           &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_cross_edges_refresh(&right, &right.repos[0], &right_edge, 1,
                                           &stats, err, sizeof(err)), 0);

    char left_master[4096];
    char right_master[4096];
    ASSERT_EQ(cbm_aosp_master_path(&left, left_master, sizeof(left_master), false), 0);
    ASSERT_EQ(cbm_aosp_master_path(&right, right_master, sizeof(right_master), false), 0);
    sqlite3 *left_db = NULL;
    sqlite3 *right_db = NULL;
    sqlite3_stmt *left_stmt = NULL;
    sqlite3_stmt *right_stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(left_master, &left_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_open_v2(right_master, &right_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(left_db, "SELECT edge_id FROM cross_symbol_edges;",
                                 -1, &left_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(right_db, "SELECT edge_id FROM cross_symbol_edges;",
                                 -1, &right_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(left_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_step(right_stmt), SQLITE_ROW);
    ASSERT(strcmp((const char *)sqlite3_column_text(left_stmt, 0),
                  (const char *)sqlite3_column_text(right_stmt, 0)) != 0);
    sqlite3_finalize(left_stmt);
    sqlite3_finalize(right_stmt);
    sqlite3_close(left_db);
    sqlite3_close(right_db);

    ASSERT_EQ(sqlite3_open(left_master, &left_db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(left_db,
        "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
        "VALUES(?1,?2,'retry_test',strftime('%s','now')) ON CONFLICT DO UPDATE SET "
        "reason=excluded.reason,queued_at=excluded.queued_at;", -1, &left_stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(left_stmt, 1, left.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(left_stmt, 2, left.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(left_stmt), SQLITE_DONE);
    sqlite3_finalize(left_stmt);
    sqlite3_close(left_db);

    left_edge.target_global_id = right_target;
    ASSERT_NEQ(cbm_aosp_cross_edges_refresh(&left, &left.repos[0], &left_edge, 1,
                                            &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_cross_edge_stats(&left, &left.repos[0], &stats,
                                        err, sizeof(err)), 0);
    ASSERT_EQ(stats.edge_count, 1);
    ASSERT_EQ(sqlite3_open_v2(left_master, &left_db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(left_db,
        "SELECT count(*) FROM cross_edge_refresh_queue WHERE workspace_id=?1 AND source_repo_id=?2;",
        -1, &left_stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(left_stmt, 1, left.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(left_stmt, 2, left.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(left_stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(left_stmt, 0), 1);
    sqlite3_finalize(left_stmt);
    sqlite3_close(left_db);

    free(left_source);
    free(left_target);
    free(right_source);
    free(right_target);
    cbm_aosp_workspace_free(&left);
    cbm_aosp_workspace_free(&right);
    th_rmtree(left_root);
    th_rmtree(right_root);
    free(left_root);
    free(right_root);
    PASS();
}

static int insert_shard_node(sqlite3_stmt *stmt, int id, const char *name,
                             const char *qualified_name, const char *label,
                             const char *file_path) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, qualified_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
}

static int create_structural_shard(const char *path, const char *project, bool source) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK ||
        sqlite3_exec(db,
            "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
            "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);",
            NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO nodes VALUES(?1,?2,?3,?4,?5,1,2,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    char qn[1024];
    int rc = 0;
    if (source) {
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.java.__file__", project);
        rc |= insert_shard_node(stmt, 1, "Client.java", qn, "File", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.client.cpp.__file__", project);
        rc |= insert_shard_node(stmt, 2, "client.cpp", qn, "File", "client.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client", project);
        rc |= insert_shard_node(stmt, 3, "Client", qn, "Class", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.convert", project);
        rc |= insert_shard_node(stmt, 4, "convert", qn, "Method", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.LocalBase", project);
        rc |= insert_shard_node(stmt, 5, "LocalBase", qn, "Class", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.LocalChild", project);
        rc |= insert_shard_node(stmt, 6, "LocalChild", qn, "Class", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.client.NativeClient", project);
        rc |= insert_shard_node(stmt, 7, "NativeClient", qn, "Class", "client.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.localOnly", project);
        rc |= insert_shard_node(stmt, 8, "localOnly", qn, "Method", "src/app/Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.client.native_client", project);
        rc |= insert_shard_node(stmt, 9, "native_client", qn, "Function", "client.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.__file__", project);
        rc |= insert_shard_node(stmt, 10, "Client.kt", qn, "File", "src/app/Client.kt");
        (void)snprintf(qn, sizeof(qn), "%s.src.app.Client.kotlin_client", project);
        rc |= insert_shard_node(stmt, 11, "kotlin_client", qn, "Function", "src/app/Client.kt");
        (void)snprintf(qn, sizeof(qn), "%s.src.client.__file__", project);
        rc |= insert_shard_node(stmt, 12, "client.rs", qn, "File", "src/client.rs");
        (void)snprintf(qn, sizeof(qn), "%s.src.client.rust_client", project);
        rc |= insert_shard_node(stmt, 13, "rust_client", qn, "Function", "src/client.rs");
    } else {
        (void)snprintf(qn, sizeof(qn), "%s.include.vendor.api.vendor.h.__file__", project);
        rc |= insert_shard_node(stmt, 1, "vendor.h", qn, "File", "include/vendor/api/vendor.h");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorBase", project);
        rc |= insert_shard_node(stmt, 2, "VendorBase", qn, "Class", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorInterface", project);
        rc |= insert_shard_node(stmt, 3, "VendorInterface", qn, "Interface", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorAnnotation", project);
        rc |= insert_shard_node(stmt, 4, "VendorAnnotation", qn, "Decorator", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorType", project);
        rc |= insert_shard_node(stmt, 5, "VendorType", qn, "Class", "src/vendor/api/Types.java");
        (void)snprintf(qn, sizeof(qn), "%s.include.vendor.api.VendorNativeBase", project);
        rc |= insert_shard_node(stmt, 6, "VendorNativeBase", qn, "Class", "include/vendor/api/vendor.h");
        (void)snprintf(qn, sizeof(qn), "%s.src.vendor.api.VendorApi.execute", project);
        rc |= insert_shard_node(stmt, 7, "execute", qn, "Method", "src/vendor/api/Calls.java");
        (void)snprintf(qn, sizeof(qn), "%s.native.vendor_run", project);
        rc |= insert_shard_node(stmt, 8, "vendor_run", qn, "Function", "native/calls.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.kotlin.vendorKotlin", project);
        rc |= insert_shard_node(stmt, 9, "vendorKotlin", qn, "Function", "kotlin/Calls.kt");
        (void)snprintf(qn, sizeof(qn), "%s.rust.vendor.rust_run", project);
        rc |= insert_shard_node(stmt, 10, "rust_run", qn, "Function", "rust/calls.rs");
        (void)snprintf(qn, sizeof(qn), "%s.values.sharedValue", project);
        rc |= insert_shard_node(stmt, 11, "sharedValue", qn, "Field", "src/vendor/api/Values.java");
        (void)snprintf(qn, sizeof(qn), "%s.values.vendor_value", project);
        rc |= insert_shard_node(stmt, 12, "vendor_value", qn, "Variable", "native/values.cpp");
        (void)snprintf(qn, sizeof(qn), "%s.values.vendorValue", project);
        rc |= insert_shard_node(stmt, 13, "vendorValue", qn, "Variable", "kotlin/Values.kt");
        (void)snprintf(qn, sizeof(qn), "%s.values.RUST_VALUE", project);
        rc |= insert_shard_node(stmt, 14, "RUST_VALUE", qn, "Variable", "rust/values.rs");
        (void)snprintf(qn, sizeof(qn), "%s.overload.A.ambiguousCall", project);
        rc |= insert_shard_node(stmt, 15, "ambiguousCall", qn, "Method", "src/vendor/api/A.java");
        (void)snprintf(qn, sizeof(qn), "%s.overload.B.ambiguousCall", project);
        rc |= insert_shard_node(stmt, 16, "ambiguousCall", qn, "Method", "src/vendor/api/B.java");
        (void)snprintf(qn, sizeof(qn), "%s.right.qualifierOnly", project);
        rc |= insert_shard_node(stmt, 17, "qualifierOnly", qn, "Function", "src/vendor/api/Only.java");
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == 0 ? 0 : -1;
}

static int set_repo_indexed(const cbm_aosp_workspace_t *workspace,
                            const cbm_aosp_repo_t *repo, const char *db_path) {
    char master_path[4096];
    if (cbm_aosp_master_path(workspace, master_path, sizeof(master_path), false) != 0) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(master_path, &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "UPDATE repos SET status='indexed',db_path=?1 WHERE workspace_id=?2 AND repo_id=?3;",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace->workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, repo->repo_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

TEST(aosp_structural_federation_collects_cross_repo_candidates_only) {
    char *root = NULL;
    ASSERT_EQ(create_workspace_fixture(&root), 0);
    ASSERT_EQ(make_dir(root, "frameworks/base/src/app"), 0);
    ASSERT_EQ(make_dir(root, "vendor/acme/widgets/include/vendor/api"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/app/Client.java",
        "package app;\n"
        "import vendor.api.VendorAnnotation;\n"
        "import vendor.api.VendorBase;\n"
        "import vendor.api.VendorInterface;\n"
        "import vendor.api.VendorType;\n"
        "@VendorAnnotation\n"
        "class Client extends VendorBase implements VendorInterface {\n"
        "  VendorType convert(VendorType value) {\n"
        "    VendorApi.execute();\n"
        "    localOnly();\n"
        "    ambiguousCall();\n"
        "    wrong.qualifierOnly();\n"
        "    missingJava();\n"
        "    return sharedValue;\n"
        "  }\n"
        "  void localOnly() {}\n"
        "}\n"
        "class LocalBase {}\n"
        "class LocalChild extends LocalBase {}\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/client.cpp",
        "#include \"vendor/api/vendor.h\"\n"
        "class NativeClient : public VendorNativeBase {};\n"
        "void native_client() { vendor_run(); missing_cpp(); int x = vendor_value; }\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/app/Client.kt",
        "package app\n"
        "fun kotlin_client() { vendorKotlin(); missingKotlin(); val x = vendorValue }\n"), 0);
    ASSERT_EQ(write_relative(root, "frameworks/base/src/client.rs",
        "fn rust_client() { vendor::rust_run(); missing_rust(); let x = RUST_VALUE; }\n"), 0);
    ASSERT_EQ(write_relative(root, "vendor/acme/widgets/include/vendor/api/vendor.h",
        "class VendorNativeBase {};\n"), 0);

    cbm_aosp_workspace_t workspace;
    char err[1024] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char source_path[4096];
    char target_path[4096];
    char source_project[64];
    char target_project[64];
    (void)snprintf(source_path, sizeof(source_path), "%s/struct-source.db", root);
    (void)snprintf(target_path, sizeof(target_path), "%s/struct-target.db", root);
    (void)snprintf(source_project, sizeof(source_project), "aosp-%s", workspace.repos[0].repo_id);
    (void)snprintf(target_project, sizeof(target_project), "aosp-%s", workspace.repos[1].repo_id);
    ASSERT_EQ(create_structural_shard(source_path, source_project, true), 0);
    ASSERT_EQ(create_structural_shard(target_path, target_project, false), 0);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[0], source_path,
                                       err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[1], target_path,
                                       err, sizeof(err)), 0);
    ASSERT_EQ(set_repo_indexed(&workspace, &workspace.repos[0], source_path), 0);
    ASSERT_EQ(set_repo_indexed(&workspace, &workspace.repos[1], target_path), 0);

    cbm_aosp_structural_stats_t stats;
    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 2);
    ASSERT(stats.files_scanned >= 2);
    ASSERT(stats.edges.resolved_count >= 6);
    ASSERT(stats.local_references_skipped >= 1);

    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open_v2(master_path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    const char *types[] = {"IMPORTS", "INCLUDES", "EXTENDS", "IMPLEMENTS", "ANNOTATED_BY", "USES_TYPE"};
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND type=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, types[i], -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT(sqlite3_column_int(stmt, 0) >= 1);
    }
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT confidence,properties FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND type='CALLS' AND target_name=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    const char *scored_targets[] = {"VendorApi.execute", "vendorKotlin"};
    const char *scored_resolutions[] = {"qualified_suffix", "unique_short_name"};
    for (size_t i = 0; i < sizeof(scored_targets) / sizeof(scored_targets[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, scored_targets[i], -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        double confidence = sqlite3_column_double(stmt, 0);
        ASSERT(i == 0 ? confidence >= 0.95 : confidence < 0.80);
        const char *properties = (const char *)sqlite3_column_text(stmt, 1);
        ASSERT_NOT_NULL(properties);
        ASSERT_NOT_NULL(strstr(properties, scored_resolutions[i]));
        ASSERT_NOT_NULL(strstr(properties, "\"candidate_count\":1"));
    }
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND target_name='LocalBase';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    const char *resolved_targets[] = {
        "VendorApi.execute", "vendor_run", "vendorKotlin", "vendor.rust_run",
        "sharedValue", "vendorValue", "RUST_VALUE",
    };
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT status,confidence,evidence FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND target_name=?2 AND type=?3;",
        -1, &stmt, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(resolved_targets) / sizeof(resolved_targets[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, resolved_targets[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, i < 4 ? "CALLS" : "USAGE", -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "resolved");
        ASSERT(sqlite3_column_double(stmt, 1) > 0.0);
        ASSERT(sqlite3_column_text(stmt, 2) != NULL);
    }
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_symbol_edges WHERE workspace_id=?1 AND type='CALLS' "
        "AND target_name='localOnly';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*),count(DISTINCT target_global_id),min(status),max(status),"
        "min(confidence),min(properties),max(properties) FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND type='CALLS' AND target_name='ambiguousCall';",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 2);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 2), "ambiguous");
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 3), "ambiguous");
    ASSERT(sqlite3_column_double(stmt, 4) < 0.5);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 5),
                  (const char *)sqlite3_column_text(stmt, 6));
    ASSERT_NOT_NULL(strstr((const char *)sqlite3_column_text(stmt, 5),
                           "\"candidate_count\":2"));
    sqlite3_finalize(stmt);

    const char *unresolved_targets[] = {
        "missingJava", "missing_cpp", "missingKotlin", "missing_rust", "wrong.qualifierOnly",
    };
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT status,target_global_id,confidence,evidence,properties FROM cross_symbol_edges "
        "WHERE workspace_id=?1 AND type='CALLS' AND target_name=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    for (size_t i = 0; i < sizeof(unresolved_targets) / sizeof(unresolved_targets[0]); i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, unresolved_targets[i], -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "unresolved");
        ASSERT_EQ(sqlite3_column_type(stmt, 1), SQLITE_NULL);
        ASSERT_EQ(sqlite3_column_double(stmt, 2), 0.0);
        ASSERT(sqlite3_column_text(stmt, 3) != NULL);
        const char *properties = (const char *)sqlite3_column_text(stmt, 4);
        ASSERT_NOT_NULL(properties);
        ASSERT_NOT_NULL(strstr(properties, "\"resolution\":\"unresolved\""));
        ASSERT_NOT_NULL(strstr(properties, "\"candidate_count\":0"));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

static int create_refresh_shard(const char *path, const char *project, int kind) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK ||
        sqlite3_exec(db,
            "DROP TABLE IF EXISTS nodes;"
            "CREATE TABLE nodes(id INTEGER PRIMARY KEY,name TEXT,qualified_name TEXT,label TEXT,"
            "file_path TEXT,start_line INTEGER,end_line INTEGER,properties TEXT);",
            NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO nodes VALUES(?1,?2,?3,?4,?5,1,3,'{}');",
            -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    char qn[1024];
    int rc = 0;
    if (kind == 0) {
        (void)snprintf(qn, sizeof(qn), "%s.Client.__file__", project);
        rc |= insert_shard_node(stmt, 1, "Client.java", qn, "File", "Client.java");
        (void)snprintf(qn, sizeof(qn), "%s.Client.run", project);
        rc |= insert_shard_node(stmt, 2, "run", qn, "Method", "Client.java");
    } else if (kind == 1) {
        (void)snprintf(qn, sizeof(qn), "%s.TargetApi.hit", project);
        rc |= insert_shard_node(stmt, 1, "hit", qn, "Method", "TargetApi.java");
    } else if (kind == 2) {
        (void)snprintf(qn, sizeof(qn), "%s.Other.__file__", project);
        rc |= insert_shard_node(stmt, 1, "Other.java", qn, "File", "Other.java");
        (void)snprintf(qn, sizeof(qn), "%s.Other.run", project);
        rc |= insert_shard_node(stmt, 2, "run", qn, "Method", "Other.java");
    } else {
        (void)snprintf(qn, sizeof(qn), "%s.Replacement", project);
        rc |= insert_shard_node(stmt, 1, "Replacement", qn, "Class", "Replacement.java");
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc == 0 ? 0 : -1;
}

TEST(aosp_federation_refreshes_only_invalidated_repositories) {
    const char *temp_root = th_mktempdir("cbm_aosp_refresh");
    ASSERT_NOT_NULL(temp_root);
    char *root = strdup(temp_root);
    ASSERT_NOT_NULL(root);
    ASSERT_EQ(make_dir(root, ".repo/manifests"), 0);
    ASSERT_EQ(make_dir(root, "source/app"), 0);
    ASSERT_EQ(make_dir(root, "target/lib"), 0);
    ASSERT_EQ(make_dir(root, "unrelated/tool"), 0);
    ASSERT_EQ(write_relative(root, ".repo/manifest.xml",
        "<manifest>"
        "<project name=\"source/app\" path=\"source/app\"/>"
        "<project name=\"target/lib\" path=\"target/lib\"/>"
        "<project name=\"unrelated/tool\" path=\"unrelated/tool\"/>"
        "</manifest>"), 0);
    ASSERT_EQ(write_relative(root, "source/app/Client.java",
        "class Client { void run() { TargetApi.hit(); } }\n"), 0);
    ASSERT_EQ(write_relative(root, "unrelated/tool/Other.java",
        "class Other { void run() { MissingOther(); } }\n"), 0);

    cbm_aosp_workspace_t workspace;
    char err[1024] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 3);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);
    char shard_paths[3][4096];
    char projects[3][64];
    for (int i = 0; i < 3; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/repo-%d.db", root, i);
        (void)snprintf(projects[i], sizeof(projects[i]), "aosp-%s", workspace.repos[i].repo_id);
        ASSERT_EQ(create_refresh_shard(shard_paths[i], projects[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(set_repo_indexed(&workspace, &workspace.repos[i], shard_paths[i]), 0);
    }

    cbm_aosp_structural_stats_t stats;
    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 3);
    char master_path[4096];
    ASSERT_EQ(cbm_aosp_master_path(&workspace, master_path, sizeof(master_path), false), 0);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT edge_id FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[2].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    char *unrelated_edge_id = strdup((const char *)sqlite3_column_text(stmt, 0));
    ASSERT_NOT_NULL(unrelated_edge_id);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(create_refresh_shard(shard_paths[1], projects[1], 3), 0);
    ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[1], shard_paths[1],
                                       err, sizeof(err)), 0);
    cbm_aosp_master_stats_t status;
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.cross_edge_count, 2);
    ASSERT_EQ(status.resolved_edge_count, 1);
    ASSERT_EQ(status.unresolved_edge_count, 1);
    ASSERT_EQ(status.stale_repo_count, 2);
    ASSERT_EQ(status.stale_edge_count, 1);
    ASSERT_EQ(status.refresh_failed_count, 0);
    ASSERT_EQ(cbm_aosp_cross_edge_refresh_failed(&workspace, &workspace.repos[0],
                                                  "fixture refresh failure",
                                                  err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.refresh_failed_count, 1);
    char status_args[8192];
    (void)snprintf(status_args, sizeof(status_args),
                   "{\"workspace_root\":\"%s\"}", root);
    char *status_response = cbm_mcp_handle_tool(NULL, "aosp_get_status", status_args);
    ASSERT_NOT_NULL(status_response);
    ASSERT_NOT_NULL(strstr(status_response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(status_response, "stale_repositories"));
    ASSERT_NOT_NULL(strstr(status_response, "stale_edges"));
    ASSERT_NOT_NULL(strstr(status_response, "refresh_failures"));
    free(status_response);
    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 2);
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT edge_id FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[2].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), unrelated_edge_id);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT status FROM cross_symbol_edges WHERE workspace_id=?1 AND source_repo_id=?2 "
        "AND target_name='TargetApi.hit';", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "unresolved");
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM cross_edge_refresh_queue WHERE workspace_id=?1;",
        -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.cross_edge_count, 2);
    ASSERT_EQ(status.resolved_edge_count, 0);
    ASSERT_EQ(status.ambiguous_edge_count, 0);
    ASSERT_EQ(status.unresolved_edge_count, 2);
    ASSERT_EQ(status.stale_repo_count, 0);
    ASSERT_EQ(status.stale_edge_count, 0);
    ASSERT_EQ(status.refresh_failed_count, 0);

    ASSERT_EQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(stats.repos_scanned, 0);
    ASSERT_EQ(sqlite3_open(master_path, &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "UPDATE repos SET db_path='/missing/federation-shard.db' "
        "WHERE workspace_id=?1 AND repo_id=?2;", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO cross_edge_refresh_queue(workspace_id,source_repo_id,reason,queued_at) "
        "VALUES(?1,?2,'failure_probe',strftime('%s','now'));", -1, &stmt, NULL), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, workspace.workspace_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, workspace.repos[0].repo_id, -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    err[0] = '\0';
    ASSERT_NEQ(cbm_aosp_structural_link(&workspace, &stats, err, sizeof(err)), 0);
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &status, err, sizeof(err)), 0);
    ASSERT_EQ(status.stale_repo_count, 1);
    ASSERT_EQ(status.refresh_failed_count, 1);
    free(unrelated_edge_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

TEST(aosp_query_plane_handles_three_repos_and_partial_workspace_failures) {
    char *root = NULL;
    ASSERT_EQ(create_q7_workspace_fixture(&root), 0);
    cbm_aosp_workspace_t workspace;
    char err[512] = {0};
    ASSERT_EQ(cbm_aosp_discover(root, &workspace, err, sizeof(err)), 0);
    ASSERT_EQ(workspace.repo_count, 4);
    ASSERT_TRUE(workspace.repos[0].exists);
    ASSERT_TRUE(workspace.repos[1].exists);
    ASSERT_TRUE(workspace.repos[2].exists);
    ASSERT_FALSE(workspace.repos[3].exists);
    ASSERT_EQ(cbm_aosp_master_sync(&workspace, err, sizeof(err)), 0);

    char shard_paths[3][4096];
    for (int i = 0; i < 3; i++) {
        (void)snprintf(shard_paths[i], sizeof(shard_paths[i]), "%s/q7-%d.db", root, i);
        ASSERT_EQ(create_q7_shard(shard_paths[i], i), 0);
        ASSERT_EQ(cbm_aosp_catalog_repo_db(&workspace, &workspace.repos[i], shard_paths[i],
                                           err, sizeof(err)), 0);
        ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[i].repo_id, shard_paths[i]), 0);
    }

    char *start_id = find_symbol_id(&workspace, "Q7Start");
    char *local_id = find_symbol_id(&workspace, "Q7Local");
    char *bridge_id = find_symbol_id(&workspace, "Q7Bridge");
    char *beta_local_id = find_symbol_id(&workspace, "Q7BetaLocal");
    char *end_id = find_symbol_id(&workspace, "Q7End");
    ASSERT_NOT_NULL(start_id);
    ASSERT_NOT_NULL(local_id);
    ASSERT_NOT_NULL(bridge_id);
    ASSERT_NOT_NULL(beta_local_id);
    ASSERT_NOT_NULL(end_id);
    ASSERT_EQ(insert_cross_edge(&workspace, "q7-edge-alpha-beta",
                                workspace.repos[0].repo_id, workspace.repos[1].repo_id,
                                local_id, bridge_id, "CALLS", 0.91, "q7_alpha_beta"), 0);
    ASSERT_EQ(insert_cross_edge(&workspace, "q7-edge-beta-gamma",
                                workspace.repos[1].repo_id, workspace.repos[2].repo_id,
                                beta_local_id, end_id, "USES_TYPE", 0.82, "q7_beta_gamma"), 0);

    cbm_aosp_trace_options_t trace_options = {
        .max_depth = 5,
        .direction = CBM_AOSP_TRACE_OUTGOING,
        .result_budget = 20,
        .cancel_flag = NULL,
    };
    cbm_aosp_trace_result_t trace;
    ASSERT_EQ(cbm_aosp_trace_path(&workspace, start_id, &trace_options, &trace,
                                  err, sizeof(err)), 0);
    ASSERT_EQ(trace.node_count, 5);
    ASSERT_EQ(trace.max_depth_reached, 4);
    ASSERT_STR_EQ(trace.nodes[0].global_id, start_id);
    ASSERT_STR_EQ(trace.nodes[1].global_id, local_id);
    ASSERT_STR_EQ(trace.nodes[1].repo_id, workspace.repos[0].repo_id);
    ASSERT_STR_EQ(trace.nodes[1].file_path, "src/Alpha.c");
    ASSERT_EQ(trace.nodes[1].start_line, 5);
    ASSERT_STR_EQ(trace.nodes[1].edge_type, "CALLS");
    ASSERT_STR_EQ(trace.nodes[1].edge_evidence, "local");
    ASSERT_EQ(trace.nodes[1].confidence, 1.0);
    ASSERT_FALSE(trace.nodes[1].cross_repo);
    ASSERT_STR_EQ(trace.nodes[2].global_id, bridge_id);
    ASSERT_STR_EQ(trace.nodes[2].repo_id, workspace.repos[1].repo_id);
    ASSERT_STR_EQ(trace.nodes[2].file_path, "src/Beta.c");
    ASSERT_EQ(trace.nodes[2].start_line, 1);
    ASSERT_STR_EQ(trace.nodes[2].edge_type, "CALLS");
    ASSERT_STR_EQ(trace.nodes[2].edge_evidence, "q7_alpha_beta");
    ASSERT_EQ(trace.nodes[2].confidence, 0.91);
    ASSERT_TRUE(trace.nodes[2].cross_repo);
    ASSERT_STR_EQ(trace.nodes[3].global_id, beta_local_id);
    ASSERT_STR_EQ(trace.nodes[3].repo_id, workspace.repos[1].repo_id);
    ASSERT_STR_EQ(trace.nodes[3].file_path, "src/Beta.c");
    ASSERT_EQ(trace.nodes[3].start_line, 9);
    ASSERT_STR_EQ(trace.nodes[3].edge_type, "CALLS");
    ASSERT_STR_EQ(trace.nodes[3].edge_evidence, "local");
    ASSERT_EQ(trace.nodes[3].confidence, 1.0);
    ASSERT_FALSE(trace.nodes[3].cross_repo);
    ASSERT_STR_EQ(trace.nodes[4].global_id, end_id);
    ASSERT_STR_EQ(trace.nodes[4].repo_id, workspace.repos[2].repo_id);
    ASSERT_STR_EQ(trace.nodes[4].file_path, "src/Gamma.c");
    ASSERT_EQ(trace.nodes[4].start_line, 1);
    ASSERT_STR_EQ(trace.nodes[4].edge_type, "USES_TYPE");
    ASSERT_STR_EQ(trace.nodes[4].edge_evidence, "q7_beta_gamma");
    ASSERT_EQ(trace.nodes[4].confidence, 0.82);
    ASSERT_TRUE(trace.nodes[4].cross_repo);
    cbm_aosp_trace_result_free(&trace);

    cbm_aosp_query_hop_t hops[4] = {0};
    for (int i = 0; i < 4; i++) {
        hops[i].kind = CBM_AOSP_QUERY_KIND_SYMBOL;
        hops[i].direction = CBM_AOSP_TRACE_OUTGOING;
    }
    cbm_aosp_query_graph_options_t query_options = {
        .hops = hops,
        .hop_count = 4,
        .max_results = 20,
    };
    cbm_aosp_query_graph_result_t query;
    ASSERT_EQ(cbm_aosp_query_graph(&workspace, start_id, &query_options, &query,
                                   err, sizeof(err)), 0);
    ASSERT_EQ(query.node_count, 5);
    ASSERT_STR_EQ(query.nodes[1].repo_id, workspace.repos[0].repo_id);
    ASSERT_STR_EQ(query.nodes[2].repo_id, workspace.repos[1].repo_id);
    ASSERT_STR_EQ(query.nodes[3].repo_id, workspace.repos[1].repo_id);
    ASSERT_STR_EQ(query.nodes[4].repo_id, workspace.repos[2].repo_id);
    ASSERT_STR_EQ(query.nodes[1].edge_evidence, "local");
    ASSERT_STR_EQ(query.nodes[2].edge_evidence, "q7_alpha_beta");
    ASSERT_STR_EQ(query.nodes[3].edge_evidence, "local");
    ASSERT_STR_EQ(query.nodes[4].edge_evidence, "q7_beta_gamma");
    cbm_aosp_query_graph_result_free(&query);

    cbm_aosp_source_snippet_t snippet;
    ASSERT_EQ(cbm_aosp_read_source_snippet(&workspace, start_id, &snippet,
                                           err, sizeof(err)), 0);
    ASSERT_STR_EQ(snippet.workspace_file_path, "alpha/src/Alpha.c");
    ASSERT_NOT_NULL(strstr(snippet.source, "Q7Start"));
    cbm_aosp_source_snippet_free(&snippet);

    cbm_aosp_master_stats_t partial_stats;
    ASSERT_EQ(cbm_aosp_master_stats(&workspace, &partial_stats, err, sizeof(err)), 0);
    ASSERT_EQ(partial_stats.repo_count, 4);
    ASSERT_EQ(partial_stats.existing_count, 3);
    ASSERT_EQ(partial_stats.missing_count, 1);
    ASSERT_EQ(partial_stats.indexed_count, 3);

    char args[8192];
    (void)snprintf(args, sizeof(args), "{\"workspace_root\":\"%s\"}", root);
    char *response = cbm_mcp_handle_tool(NULL, "aosp_get_status", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(response, "repositories_missing"));
    ASSERT_NOT_NULL(strstr(response, "repositories_indexed"));
    free(response);

    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"start\":\"Duplicate\","
                   "\"max_depth\":3}", root);
    response = cbm_mcp_handle_tool(NULL, "aosp_trace_path", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(response, "ambiguous symbol reference"));
    free(response);

    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"start\":\"Q7Start\","
                   "\"hops\":[{\"kind\":\"symbol\"},{\"kind\":\"symbol\"},"
                   "{\"kind\":\"symbol\"},{\"kind\":\"symbol\"}],"
                   "\"max_results\":20}", root);
    response = cbm_mcp_handle_tool(NULL, "aosp_query_graph", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"isError\":false"));
    ASSERT_NOT_NULL(strstr(response, "q7_alpha_beta"));
    ASSERT_NOT_NULL(strstr(response, "q7_beta_gamma"));
    ASSERT_NOT_NULL(strstr(response, workspace.repos[2].repo_id));
    free(response);

    char missing_shard[4096];
    (void)snprintf(missing_shard, sizeof(missing_shard), "%s/missing-shard.db", root);
    ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[1].repo_id, missing_shard), 0);
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"global_id\":\"%s\"}",
                   root, bridge_id);
    response = cbm_mcp_handle_tool(NULL, "aosp_get_source_snippet", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(response, "cannot open AOSP repository shard"));
    free(response);
    ASSERT_EQ(mark_repo_indexed(&workspace, workspace.repos[1].repo_id, shard_paths[1]), 0);

    sqlite3 *stale_shard = NULL;
    ASSERT_EQ(sqlite3_open(shard_paths[2], &stale_shard), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(stale_shard,
                           "UPDATE nodes SET qualified_name='changed.Q7End' WHERE id=1;",
                           NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(stale_shard);
    (void)snprintf(args, sizeof(args),
                   "{\"workspace_root\":\"%s\",\"global_id\":\"%s\"}",
                   root, end_id);
    response = cbm_mcp_handle_tool(NULL, "aosp_get_source_snippet", args);
    ASSERT_NOT_NULL(response);
    ASSERT_NOT_NULL(strstr(response, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(response, "catalog is stale"));
    free(response);

    free(start_id);
    free(local_id);
    free(bridge_id);
    free(beta_local_id);
    free(end_id);
    cbm_aosp_workspace_free(&workspace);
    th_rmtree(root);
    free(root);
    PASS();
}

SUITE(aosp) {
    RUN_TEST(aosp_manifest_include_and_local_override);
    RUN_TEST(aosp_manifest_rejects_parent_path);
    RUN_TEST(aosp_manifest_keeps_duplicate_names_and_extend_path);
    RUN_TEST(aosp_workspace_ids_are_root_scoped);
    RUN_TEST(aosp_master_sync_and_stats);
    RUN_TEST(aosp_catalog_and_global_symbol_search);
    RUN_TEST(aosp_workspace_symbol_resolver_tiers_and_ambiguity);
    RUN_TEST(aosp_shard_routing_routes_symbols_and_reads_nodes_and_edges);
    RUN_TEST(aosp_workspace_search_routes_to_exact_source_snippets);
    RUN_TEST(aosp_trace_path_traverses_local_and_cross_repo_edges);
    RUN_TEST(aosp_query_graph_traverses_multi_hop_code_module_protocol);
    RUN_TEST(aosp_build_graph_resolves_cross_repo_modules);
    RUN_TEST(aosp_build_graph_expands_defaults_with_provenance_and_cycles);
    RUN_TEST(aosp_build_graph_models_conditional_variants);
    RUN_TEST(aosp_build_graph_models_namespaces_packages_and_visibility);
    RUN_TEST(aosp_build_graph_models_filegroups_genrules_and_output_tags);
    RUN_TEST(aosp_build_graph_evaluates_common_android_make_semantics);
    RUN_TEST(aosp_build_graph_models_products_board_configs_and_partition_ownership);
    RUN_TEST(aosp_build_graph_imports_bazel_mixed_build_metadata);
    RUN_TEST(aosp_build_graph_links_files_generated_outputs_and_definition_symbols);
    RUN_TEST(aosp_build_queries_expose_edges_variants_gaps_and_provenance);
    RUN_TEST(aosp_protocol_graph_models_complete_aidl_declarations);
    RUN_TEST(aosp_protocol_graph_links_binder_transaction_flow);
    RUN_TEST(aosp_protocol_graph_models_generated_binder_backends);
    RUN_TEST(aosp_protocol_graph_links_service_manager_paths);
    RUN_TEST(aosp_protocol_graph_links_binder_and_jni_evidence);
    RUN_TEST(aosp_cross_edges_are_deterministic_and_refresh_per_source_repo);
    RUN_TEST(aosp_cross_edges_enforce_workspace_and_repository_boundaries);
    RUN_TEST(aosp_cross_edges_migrate_v3_schema_without_losing_resolved_edges);
    RUN_TEST(aosp_structural_federation_collects_cross_repo_candidates_only);
    RUN_TEST(aosp_federation_refreshes_only_invalidated_repositories);
    RUN_TEST(aosp_query_plane_handles_three_repos_and_partial_workspace_failures);
}
