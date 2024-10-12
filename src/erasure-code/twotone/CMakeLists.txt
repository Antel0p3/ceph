#twotone plugin

include_directories(.)

set(twotone_utils_srcs
  ${CMAKE_SOURCE_DIR}/src/erasure-code/ErasureCode.cc 
  ErasureCodePluginTwotone.cc 
  ErasureCodeTwotone.cc 
  )

add_library(twotone_utils OBJECT ${twotone_utils_srcs})

set(ec_twotone_objs
  $<TARGET_OBJECTS:gf-complete_objs>
  $<TARGET_OBJECTS:jerasure_objs>
  $<TARGET_OBJECTS:twotone_utils>)

add_library(ec_twotone SHARED ${ec_twotone_objs})
set_target_properties(ec_twotone PROPERTIES
  INSTALL_RPATH "")
target_link_libraries(ec_twotone ${EXTRALIBS})
install(TARGETS ec_twotone DESTINATION ${erasure_plugin_dir})

# legacy libraries
foreach(flavor ${jerasure_legacy_flavors})
  set(plugin_name "ec_twotone_${flavor}")
  add_library(${plugin_name} SHARED ${ec_twotone_objs})
  set_target_properties(${plugin_name} PROPERTIES
    INSTALL_RPATH "")
  install(TARGETS ${plugin_name} DESTINATION ${erasure_plugin_dir})
  add_dependencies(ec_twotone ${plugin_name})
endforeach()
