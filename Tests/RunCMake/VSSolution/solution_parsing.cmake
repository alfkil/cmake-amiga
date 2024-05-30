macro(error text)
  set(RunCMake_TEST_FAILED "${text}")
  return()
endmacro()


macro(parseGlobalSections arg_out_pre arg_out_post testName)
  set(out_pre ${arg_out_pre})
  set(out_post ${arg_out_post})
  set(sln "${RunCMake_TEST_BINARY_DIR}/${testName}.sln")
  if(NOT EXISTS "${sln}")
    error("Expected solution file ${sln} does not exist")
  endif()
  file(STRINGS "${sln}" lines)
  set(sectionLines "")
  set(store FALSE)
  foreach(line IN LISTS lines)
    if(line MATCHES "^\t*Global\n?$")
      set(store TRUE)
    elseif(line MATCHES "^\t*EndGlobal\n?$")
      set(store FALSE)
    elseif(store)
      list(APPEND sectionLines "${line}")
    endif()
  endforeach()
  set(sectionName "")
  set(sectionType "")
  foreach(line IN LISTS sectionLines)
    if(line MATCHES "^\t*GlobalSection\\((.*)\\) *= *(pre|post)Solution\n?$")
      set(sectionName "${CMAKE_MATCH_1}")
      set(sectionType ${CMAKE_MATCH_2})
      list(APPEND ${out_${sectionType}} "${sectionName}")
      if(DEFINED ${out_${sectionType}}_${sectionName})
        error("Section ${sectionName} defined twice")
      endif()
      set(${out_${sectionType}}_${sectionName} "")
    elseif(line MATCHES "\t*EndGlobalSection\n?$")
      set(sectionName "")
      set(sectionType "")
    elseif(sectionName)
      string(REGEX MATCH "^\t*([^=]*)=([^\n]*)\n?$" matches "${line}")
      if(NOT matches)
        error("Bad syntax in solution file: '${line}'")
      endif()
      string(STRIP "${CMAKE_MATCH_1}" key)
      string(STRIP "${CMAKE_MATCH_2}" value)
      if(key STREQUAL "SolutionGuid" AND value MATCHES "^{[0-9A-F-]+}$")
        set(value "{00000000-0000-0000-0000-000000000000}")
      endif()
      list(APPEND ${out_${sectionType}}_${sectionName} "${key}=${value}")
    endif()
  endforeach()
endmacro()


macro(getProjectNames arg_out_projects)
  set(${arg_out_projects} "")
  set(sln "${RunCMake_TEST_BINARY_DIR}/${test}.sln")
  if(NOT EXISTS "${sln}")
    error("Expected solution file ${sln} does not exist")
  endif()
  file(STRINGS "${sln}" project_lines REGEX "^Project\\(")
  foreach(project_line IN LISTS project_lines)
    string(REGEX REPLACE ".* = \"" "" project_line "${project_line}")
    string(REGEX REPLACE "\", .*"  "" project_line "${project_line}")
    list(APPEND ${arg_out_projects} "${project_line}")
  endforeach()
endmacro()


macro(testGlobalSection prefix sectionName)
  if(NOT DEFINED ${prefix}_${sectionName})
    error("Section ${sectionName} does not exist")
  endif()
  if(NOT "${${prefix}_${sectionName}}" STREQUAL "${ARGN}")
    error("Section ${sectionName} content mismatch\n  expected: ${ARGN}\n  actual: ${${prefix}_${sectionName}}")
  endif()
endmacro()
