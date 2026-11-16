# Strict warning policy for Oviora-owned targets only (Playbook section 5.3).
# Third-party headers are never compiled through this function.

function(oviora_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /WX)
  else()
    message(FATAL_ERROR "Oviora V1 only supports MSVC on Windows x64")
  endif()
endfunction()
