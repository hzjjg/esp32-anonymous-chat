idf_component_register(SRCS "main.c"
                           "rest_server.c"
                           "chat_server.c"
                           "chat_storage.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "../front/dist/index.html"
                                   "../front/dist/icon.png"
                                   "../front/dist/assets/index.js"
                                   "../front/dist/assets/index.css")

if(CONFIG_EXAMPLE_WEB_DEPLOY_SF)
    set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../front")
    if(EXISTS ${WEB_SRC_DIR}/dist)
        spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist FLASH_IN_PROJECT)
    else()
        message(FATAL_ERROR "${WEB_SRC_DIR}/dist doesn't exit. Please run 'npm run build' in ${WEB_SRC_DIR}")
    endif()
endif()
