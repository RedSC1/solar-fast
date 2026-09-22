set(SF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(SF_BENCH_SRCS "bench.c")
set(SF_BENCH_INCLUDES "." "${SF_ROOT}" "${SF_ROOT}/include"
    "${SF_ROOT}/core" "${SF_ROOT}/rise" "${SF_ROOT}/qishuo"
    "${SF_ROOT}/calendar" "${SF_ROOT}/bazi")
option(SF_ENABLE_LOCAL_SPA "Build optional local SPA comparison" ON)
if(SF_ENABLE_LOCAL_SPA AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/spa/spa.c" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/spa/spa.h")
    list(APPEND SF_BENCH_SRCS "spa/spa.c")
    list(APPEND SF_BENCH_INCLUDES "spa")
endif()

idf_component_register(
    SRCS ${SF_BENCH_SRCS}
         # ---- core ----
         "${SF_ROOT}/core/sf_fixed.c"
         "${SF_ROOT}/core/sf_series.c"
         "${SF_ROOT}/core/sf_solve.c"
         # ---- rise ----
         "${SF_ROOT}/rise/sf_rise.c"
         # ---- qishuo ----
         "${SF_ROOT}/qishuo/sf_qishuo.c"
         # ---- calendar ----
         "${SF_ROOT}/calendar/sf_calendar.c"
         "${SF_ROOT}/calendar/sf_ganzhi.c"
         # ---- bazi ----
         "${SF_ROOT}/bazi/sf_bazi.c"
         "${SF_ROOT}/bazi/sf_bazi_rules.c"
    INCLUDE_DIRS ${SF_BENCH_INCLUDES}
)

if(SF_ENABLE_LOCAL_SPA AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/spa/spa.c" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/spa/spa.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SF_HAVE_LOCAL_SPA=1)
endif()
