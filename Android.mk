LOCAL_PATH := $(call my-dir)
CLANG_ROOT_PATH := $(LOCAL_PATH)

include $(CLEAR_VARS)

subdirs := $(addprefix $(LOCAL_PATH)/,$(addsuffix /Android.mk, \
  lib/Analysis \
  lib/AST \
  lib/ARCMigrate \
  lib/Basic \
  lib/CodeGen \
  lib/Driver \
  lib/Edit \
  lib/Frontend \
  lib/FrontendTool \
  lib/Headers \
  lib/Lex \
  lib/Parse \
  lib/Rewrite/Core \
  lib/Rewrite/Frontend \
  lib/Sema \
  lib/Serialization \
  lib/StaticAnalyzer/Checkers \
  lib/StaticAnalyzer/Core \
  lib/StaticAnalyzer/Frontend \
  tools/driver \
  utils/TableGen \
  ))

LOCAL_CFLAGS := $(call-cc-option,-Qunused-arguments)

LOCAL_CFLAGS += -fno-strict-aliasing

include $(LOCAL_PATH)/clang.mk
include $(LOCAL_PATH)/host_shared_clang.mk

include $(subdirs)
