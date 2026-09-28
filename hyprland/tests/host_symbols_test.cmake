execute_process(
  COMMAND "${NM}" -D --defined-only "${PLUGIN}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Cannot inspect plugin symbols: ${error}")
endif()
foreach(symbol IN ITEMS g_pCompositor g_pHyprRenderer _ZN6Render2GL13g_pHyprOpenGLE)
  if(NOT symbols MATCHES "[ \t][Vu][ \t]+${symbol}([\r\n]|$)")
    message(FATAL_ERROR "${symbol} must be visible and preemptible by the Hyprland host")
  endif()
endforeach()
