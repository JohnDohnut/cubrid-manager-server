# Writes OUT: the sha256 of each HA node-adding script in SCRIPT_DIR as a C string constant.
# Run by the build (cmake -P) whenever a script changes. A script that is missing gets an empty value, which
# the manager refuses to run with.
set(content "/* generated from the scripts in server/scripts: do not edit */\n")
string(APPEND content "#ifndef _CM_HA_SCRIPT_HASH_H_\n#define _CM_HA_SCRIPT_HASH_H_\n\n")
foreach(script ${SCRIPTS})
  set(hash "")
  if(EXISTS "${SCRIPT_DIR}/${script}")
    file(SHA256 "${SCRIPT_DIR}/${script}" hash)
  endif()
  string(REGEX REPLACE "\\.sh$" "" name "${script}")
  string(TOUPPER "${name}" name)
  string(APPEND content "#define ${name}_SHA256 \"${hash}\"\n")
endforeach()
string(APPEND content "\n#endif /* _CM_HA_SCRIPT_HASH_H_ */\n")
file(WRITE "${OUT}" "${content}")
