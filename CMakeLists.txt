cmake_minimum_required(VERSION 3.22)

project(ToneForge VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE_DIR : passé par le CI ; en local, ../JUCE.
set(JUCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../JUCE CACHE PATH "Dossier JUCE")
add_subdirectory(${JUCE_DIR} JUCE)

set(NAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore)
file(GLOB NAM_SOURCES ${NAM_DIR}/NAM/*.cpp ${NAM_DIR}/NAM/*/*.cpp)

# iPhone : application autonome seulement ; captures et IR copiées par l'app Fichiers.
if (IOS)
    set(TF_FORMATS Standalone)
else()
    set(TF_FORMATS VST3 Standalone)
endif()

juce_add_plugin(ToneForge
    COMPANY_NAME "Atlas Audio"
    BUNDLE_ID "com.atlasaudio.toneforge"
    PLUGIN_MANUFACTURER_CODE Atls
    PLUGIN_CODE Tnfg
    FORMATS ${TF_FORMATS}
    PRODUCT_NAME "ToneForge"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "ToneForge lit l'entrée guitare de ta carte son."
    BACKGROUND_AUDIO_ENABLED TRUE
    FILE_SHARING_ENABLED TRUE
    DOCUMENT_BROWSER_ENABLED TRUE
    STATUS_BAR_HIDDEN TRUE
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight)

target_sources(ToneForge
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/PhoneEditor.cpp)

# Effets Airwindows (MIT), compilés tels quels avec un shim du SDK VST2 (Source/aw).
# Chaque .cpp définit createEffectInstance : renommé par effet pour éviter les doublons.
set(AW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/airwindows/plugins/WinVST)
foreach(fx Pressure5 ChorusEnsemble TapeDelay2 Galactic3)
    set(src ${AW_DIR}/${fx}/${fx}.cpp ${AW_DIR}/${fx}/${fx}Proc.cpp)
    target_sources(ToneForge PRIVATE ${src})
    set_source_files_properties(${src} PROPERTIES
        COMPILE_DEFINITIONS createEffectInstance=createEffectInstance_${fx})
    if (MSVC)
        set_source_files_properties(${src} PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4305;/wd4996")
    endif()
endforeach()

# NAM enregistre ses architectures par des initialiseurs statiques : liées dans la lib statique
# SharedCode, l'éditeur de liens les jetterait ("No config parser registered"). On lie donc
# les objets NAM directement dans chaque cible finale.
add_library(nam_obj OBJECT ${NAM_SOURCES})
target_include_directories(nam_obj PRIVATE ${NAM_DIR} ${NAM_DIR}/Dependencies/nlohmann SYSTEM PRIVATE ${NAM_DIR}/Dependencies/eigen)
target_compile_definitions(nam_obj PRIVATE NAM_SAMPLE_FLOAT NAM_ENABLE_A2_FAST NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(nam_obj PRIVATE /bigobj $<$<CONFIG:Release>:/O2>)
endif()
foreach(f ${TF_FORMATS})
    target_sources(ToneForge_${f} PRIVATE $<TARGET_OBJECTS:nam_obj>)
endforeach()

target_include_directories(ToneForge
    PRIVATE
        ${NAM_DIR}
        ${NAM_DIR}/Dependencies/nlohmann
        Source/aw
    SYSTEM PRIVATE
        ${NAM_DIR}/Dependencies/eigen)

target_compile_definitions(ToneForge
    PRIVATE
        NAM_SAMPLE_FLOAT
        NAM_ENABLE_A2_FAST
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    PUBLIC
        JUCE_ASIO=$<BOOL:${WIN32}>
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
        juce::juce_osc
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
