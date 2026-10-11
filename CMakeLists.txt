cmake_minimum_required(VERSION 3.22)

project(ToneForge VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../JUCE JUCE)

set(NAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore)
file(GLOB NAM_SOURCES ${NAM_DIR}/NAM/*.cpp ${NAM_DIR}/NAM/*/*.cpp)

juce_add_plugin(ToneForge
    COMPANY_NAME "Atlas Audio"
    BUNDLE_ID "com.atlasaudio.toneforge"
    PLUGIN_MANUFACTURER_CODE Atls
    PLUGIN_CODE Tnfg
    FORMATS VST3 Standalone
    PRODUCT_NAME "ToneForge")

target_sources(ToneForge
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

# NAM enregistre ses architectures par des initialiseurs statiques : liées dans la lib statique
# SharedCode, l'éditeur de liens les jetterait ("No config parser registered"). On lie donc
# les objets NAM directement dans chaque cible finale.
add_library(nam_obj OBJECT ${NAM_SOURCES})
target_include_directories(nam_obj PRIVATE ${NAM_DIR} ${NAM_DIR}/Dependencies/nlohmann SYSTEM PRIVATE ${NAM_DIR}/Dependencies/eigen)
target_compile_definitions(nam_obj PRIVATE NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(nam_obj PRIVATE /bigobj $<$<CONFIG:Release>:/O2>)
endif()
foreach(t ToneForge_VST3 ToneForge_Standalone)
    target_sources(${t} PRIVATE $<TARGET_OBJECTS:nam_obj>)
endforeach()

target_include_directories(ToneForge
    PRIVATE
        ${NAM_DIR}
        ${NAM_DIR}/Dependencies/nlohmann
    SYSTEM PRIVATE
        ${NAM_DIR}/Dependencies/eigen)

target_compile_definitions(ToneForge
    PRIVATE
        NAM_SAMPLE_FLOAT
        NAM_ENABLE_A2_FAST
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

if (MSVC)
    target_compile_options(ToneForge PRIVATE /bigobj)
endif()

target_link_libraries(ToneForge
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
