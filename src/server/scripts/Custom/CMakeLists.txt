set(scripts_STAT_SRCS
  ${scripts_STAT_SRCS}
)

AC_ADD_SCRIPT_LOADER("Custom" "ScriptLoader.h")

message("  -> Prepared: Custom scripts")