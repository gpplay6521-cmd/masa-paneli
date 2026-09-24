file(GLOB FONT_SRCS "fonts/*.c")
# install/MasaPaneli-Kurulum.cmd: panelin bilgisayara indirttiği kurulum dosyası (pc-helper\paketle.ps1 -Hafif üretir); yazılıma gömülür.
# (main bileşeni REQUIRES verilmeyince tüm bileşenleri görür; esp_wifi, esp_http_server ve components\dns_server böyle bulunur.)
idf_component_register(
    SRCS "main.c" "board.c" "settings.c" "proto.c" "theme.c" "ui.c" "portal.c" ${FONT_SRCS}
    INCLUDE_DIRS "." "fonts"
    EMBED_FILES "install/MasaPaneli-Kurulum.cmd")
