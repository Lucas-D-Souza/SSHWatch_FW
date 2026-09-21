#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <string.h>

static const char *TAG = "SSH_Client";
#define BOOT_BTN_PIN GPIO_NUM_0

// ==========================================
// VARIÁVEIS GLOBAIS E DE TELA
// ==========================================
LV_IMAGE_DECLARE(icon_ssh); 

static lv_obj_t * scr_splash = NULL;
static lv_obj_t * scr_wifi_list = NULL;
static lv_obj_t * scr_password = NULL;
static lv_obj_t * scr_ssh_config = NULL;
static lv_obj_t * scr_terminal = NULL;

// Componentes Wi-Fi
static lv_obj_t * list_wifi = NULL;
static lv_obj_t * ta_wifi_pass = NULL;
static lv_obj_t * label_wifi_title = NULL;
static char current_ssid[33] = {0};

// Componentes SSH Config
static lv_obj_t * lbl_local_ip = NULL;
static lv_obj_t * ta_host = NULL;
static lv_obj_t * ta_user = NULL;
static lv_obj_t * ta_pass = NULL;
static lv_obj_t * dd_keys = NULL;
static lv_obj_t * switch_auth = NULL;

// Componentes Terminal
static lv_obj_t * ta_log = NULL;
static lv_obj_t * ta_input = NULL;
static lv_obj_t * kb_terminal = NULL;

// ==========================================
// FUNÇÕES DE HARDWARE E SISTEMA
// ==========================================
static void return_to_factory() {
    ESP_LOGI(TAG, "Retornando ao Factory Firmware...");
    if (lvgl_port_lock(0)) {
        bsp_display_brightness_set(0); 
        lvgl_port_unlock();
    }
    const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        esp_restart();
    }
}

static void clear_i2c_bus(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(GPIO_NUM_14, 0); esp_rom_delay_us(100);
        gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    }
    gpio_set_level(GPIO_NUM_15, 0); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);

    gpio_reset_pin(GPIO_NUM_14);
    gpio_reset_pin(GPIO_NUM_15);
}

// ==========================================
// SPLASH SCREEN
// ==========================================
static void show_splash_screen(const char* version) {
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(scr_splash, LV_SCROLLBAR_MODE_OFF); // CORRIGIDO PARA LVGL 9.4

    lv_obj_t * logo = lv_image_create(scr_splash);
    lv_image_set_src(logo, &icon_ssh);
    lv_image_set_scale(logo, 512);
    lv_obj_set_size(logo, 200, 200);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -20); 

    lv_obj_t * lbl_version = lv_label_create(scr_splash);
    lv_label_set_text_fmt(lbl_version, "v%s", version);
    lv_obj_set_style_text_font(lbl_version, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_version, lv_color_hex(0x555555), 0); 
    lv_obj_align(lbl_version, LV_ALIGN_BOTTOM_MID, 0, -25);

    lv_screen_load(scr_splash);
}

// ==========================================
// INTERFACE: WI-FI LISTA & SENHA
// ==========================================
static void start_wifi_scan(void) {
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&scan_config, false);
}

static void wifi_list_item_click_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 1); 
    if (label) {
        const char * text = lv_label_get_text(label);
        const char * paren = strrchr(text, '(');
        int len = (paren != NULL && paren > text) ? (paren - text - 1) : 32; 
        if (len > 32) len = 32;
        strncpy(current_ssid, text, len);
        current_ssid[len] = '\0';

        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "Senha: %s", current_ssid);
        lv_label_set_text(label_wifi_title, title_buf);
        lv_textarea_set_text(ta_wifi_pass, ""); 
        lv_scr_load_anim(scr_password, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

static void build_wifi_ui() {
    // Tela Lista
    scr_wifi_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi_list, lv_color_black(), 0);
    
    lv_obj_t * header = lv_label_create(scr_wifi_list);
    lv_label_set_text(header, "Redes Wi-Fi"); 
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 30); 

    list_wifi = lv_list_create(scr_wifi_list);
    lv_obj_set_size(list_wifi, 310, 380); 
    lv_obj_align(list_wifi, LV_ALIGN_TOP_MID, 0, 70); 
    lv_obj_set_style_bg_color(list_wifi, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_wifi, 0, 0);

    lv_obj_t * txt = lv_list_add_text(list_wifi, "Buscando...");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);

    // Tela Senha
    scr_password = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_password, lv_color_black(), 0);

    label_wifi_title = lv_label_create(scr_password);
    lv_obj_set_style_text_color(label_wifi_title, lv_color_white(), 0); 
    lv_obj_set_style_text_font(label_wifi_title, &lv_font_montserrat_20, 0);
    lv_label_set_text(label_wifi_title, "Senha:");
    lv_obj_align(label_wifi_title, LV_ALIGN_TOP_LEFT, 50, 30);

    ta_wifi_pass = lv_textarea_create(scr_password);
    lv_textarea_set_password_mode(ta_wifi_pass, false); 
    lv_textarea_set_one_line(ta_wifi_pass, true);
    lv_obj_set_width(ta_wifi_pass, 310);
    lv_obj_align(ta_wifi_pass, LV_ALIGN_TOP_MID, 0, 70); 

    lv_obj_t * kb = lv_keyboard_create(scr_password);
    lv_keyboard_set_textarea(kb, ta_wifi_pass);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x111111), LV_PART_MAIN); 
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x333333), LV_PART_ITEMS); 
    lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS); 
    
    lv_obj_add_event_cb(kb, [](lv_event_t * e) {
        lv_label_set_text(label_wifi_title, "Conectando...");
        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.sta.ssid, current_ssid, 32);
        strncpy((char *)wifi_config.sta.password, lv_textarea_get_text(ta_wifi_pass), 64);
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
    }, LV_EVENT_READY, NULL);

    lv_obj_add_event_cb(kb, [](lv_event_t * e) {
        lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }, LV_EVENT_CANCEL, NULL);
}

// ==========================================
// INTERFACE: TERMINAL DE COMANDO
// ==========================================
static void terminal_kb_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * kb = (lv_obj_t*)lv_event_get_target(e);
    if(code == LV_EVENT_READY) {
        // Pega o comando digitado
        const char * cmd = lv_textarea_get_text(ta_input);
        
        // Joga pro log superior simulando o envio
        lv_textarea_add_text(ta_log, "\nroot@server:~# ");
        lv_textarea_add_text(ta_log, cmd);
        
        // Limpa a caixinha pra digitar de novo
        lv_textarea_set_text(ta_input, ""); 
    }
}

static void build_terminal_ui() {
    scr_terminal = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_terminal, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(scr_terminal, LV_SCROLLBAR_MODE_OFF);

    // 1. Área de Log (Leitura)
    ta_log = lv_textarea_create(scr_terminal);
    lv_obj_set_size(ta_log, 390, 420); // Ocupa quase a tela toda
    lv_obj_align(ta_log, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(ta_log, lv_color_black(), 0);
    lv_obj_set_style_text_color(ta_log, lv_color_hex(0x00FF00), 0); // Verde Hacker
    lv_obj_set_style_border_width(ta_log, 0, 0);
    lv_textarea_set_cursor_click_pos(ta_log, false);
    lv_textarea_set_text(ta_log, "Conectado com sucesso!\nSSHWatch OS v1.0\nType commands below.\n");

    // 2. Caixa de Input (Onde o usuário clica para digitar)
    ta_input = lv_textarea_create(scr_terminal);
    lv_obj_set_size(ta_input, 390, 45);
    lv_obj_align(ta_input, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(ta_input, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(ta_input, lv_color_white(), 0);
    lv_obj_set_style_border_width(ta_input, 1, 0);
    lv_textarea_set_one_line(ta_input, true);
    lv_textarea_set_placeholder_text(ta_input, "> Digite um comando...");

    // 3. Teclado Virtual (Inicia escondido)
    kb_terminal = lv_keyboard_create(scr_terminal);
    lv_keyboard_set_textarea(kb_terminal, ta_input);
    lv_obj_add_flag(kb_terminal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(kb_terminal, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_color(kb_terminal, lv_color_hex(0x444444), LV_PART_ITEMS); 
    lv_obj_add_event_cb(kb_terminal, terminal_kb_event_cb, LV_EVENT_READY, NULL);

    // ==========================================
    // A MÁGICA DA UX POR FOCO
    // ==========================================
    // Quando focar no Input (Clicou para digitar)
    lv_obj_add_event_cb(ta_input, [](lv_event_t * e) {
        lv_obj_remove_flag(kb_terminal, LV_OBJ_FLAG_HIDDEN); // Mostra o teclado
        lv_obj_set_height(ta_log, 160);  // Encolhe o log para cima
        lv_obj_align(ta_input, LV_ALIGN_TOP_MID, 0, 180); // Sobe o input pra cima do teclado
    }, LV_EVENT_FOCUSED, NULL);

    // Quando clicar no Log (Quer ler a tela toda)
    lv_obj_add_event_cb(ta_log, [](lv_event_t * e) {
        lv_obj_add_flag(kb_terminal, LV_OBJ_FLAG_HIDDEN); // Esconde o teclado
        lv_obj_set_height(ta_log, 420); // Estica o log até o fundo
        lv_obj_align(ta_input, LV_ALIGN_BOTTOM_MID, 0, -10); // Desce a caixa de texto
        lv_obj_remove_state(ta_input, LV_STATE_FOCUSED); // Tira o cursor
    }, LV_EVENT_CLICKED, NULL);

    // Botão Sair flutuante no Log (Escondido)
    lv_obj_t * btn_exit = lv_button_create(scr_terminal);
    lv_obj_set_size(btn_exit, 50, 50);
    lv_obj_align(btn_exit, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0xAA0000), 0);
    lv_obj_set_style_bg_opa(btn_exit, LV_OPA_50, 0); // Meio transparente
    lv_obj_t * lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, LV_SYMBOL_POWER);
    lv_obj_center(lbl_exit);
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e){ return_to_factory(); }, LV_EVENT_CLICKED, NULL);
}

// ==========================================
// INTERFACE: SSH CONFIG (TAB VIEW)
// ==========================================
static void btn_connect_event_cb(lv_event_t * e) {
    // Cria um loading spinner na tela atual e trava interações
    lv_obj_t * spinner = lv_spinner_create(scr_ssh_config);
    lv_obj_set_size(spinner, 100, 100);
    lv_obj_center(spinner);
    lv_spinner_set_anim_params(spinner, 1000, 60);

    // Simula tempo de handshake e depois vai pro terminal
    lv_timer_create([](lv_timer_t * t) {
        lv_scr_load_anim(scr_terminal, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
        lv_timer_delete(t);
    }, 2000, NULL);
}

static void build_ssh_config_ui() {
    scr_ssh_config = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ssh_config, lv_color_black(), 0);

    // Barra indicadora de IP no topo da tela inteira
    lbl_local_ip = lv_label_create(scr_ssh_config);
    lv_label_set_text(lbl_local_ip, "#AAAAAA Rede:# Aguardando IP...");
    lv_label_set_text_selection_bg_color(lbl_local_ip, lv_color_hex(0x00FF00));
    lv_label_set_long_mode(lbl_local_ip, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(lbl_local_ip, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_local_ip, LV_ALIGN_TOP_LEFT, 10, 10);

    // Botão Sair no canto superior
    lv_obj_t * btn_exit = lv_button_create(scr_ssh_config);
    lv_obj_set_size(btn_exit, 40, 40);
    lv_obj_align(btn_exit, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0x880000), 0);
    lv_obj_t * lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, LV_SYMBOL_POWER);
    lv_obj_center(lbl_exit);
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e){ return_to_factory(); }, LV_EVENT_CLICKED, NULL);

    // O TAB VIEW
    lv_obj_t * tv = lv_tabview_create(scr_ssh_config);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, 50);
    lv_obj_set_size(tv, 410, 440); // Resto da tela
    lv_obj_align(tv, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);

    lv_obj_t * tab_new = lv_tabview_add_tab(tv, "Nova Conexão");
    lv_obj_t * tab_saved = lv_tabview_add_tab(tv, "Salvos");

    // ------------------------------------------
    // ABA 1: NOVA CONEXÃO (FLEXBOX)
    // ------------------------------------------
    lv_obj_set_flex_flow(tab_new, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab_new, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_new, 10, 0);
    lv_obj_set_style_pad_row(tab_new, 15, 0);

    ta_host = lv_textarea_create(tab_new);
    lv_textarea_set_one_line(ta_host, true);
    lv_textarea_set_placeholder_text(ta_host, "IP ou Hostname");
    lv_obj_set_width(ta_host, 340);

    ta_user = lv_textarea_create(tab_new);
    lv_textarea_set_one_line(ta_user, true);
    lv_textarea_set_placeholder_text(ta_user, "Usuario (ex: root)");
    lv_obj_set_width(ta_user, 340);

    ta_pass = lv_textarea_create(tab_new);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_textarea_set_placeholder_text(ta_pass, "Senha");
    lv_obj_set_width(ta_pass, 340);

    // Row para Switch de Chave e Porta
    lv_obj_t * row_opt = lv_obj_create(tab_new);
    lv_obj_set_size(row_opt, 340, 50);
    lv_obj_set_style_bg_opa(row_opt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_opt, 0, 0);
    lv_obj_set_flex_flow(row_opt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_opt, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    switch_auth = lv_switch_create(row_opt);
    lv_obj_t * lbl_sw = lv_label_create(row_opt);
    lv_label_set_text(lbl_sw, "Usar Chave SD");
    lv_obj_set_style_text_color(lbl_sw, lv_color_white(), 0);

    lv_obj_t * ta_port = lv_textarea_create(row_opt);
    lv_textarea_set_one_line(ta_port, true);
    lv_textarea_set_text(ta_port, "22");
    lv_obj_set_width(ta_port, 80);

    // Botão de Conectar
    lv_obj_t * btn_connect = lv_button_create(tab_new);
    lv_obj_set_size(btn_connect, 200, 60);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0x007BFF), 0);
    lv_obj_set_style_radius(btn_connect, 30, 0);
    lv_obj_t * lbl_conn = lv_label_create(btn_connect);
    lv_label_set_text(lbl_conn, "Conectar");
    lv_obj_set_style_text_font(lbl_conn, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_conn);
    lv_obj_add_event_cb(btn_connect, btn_connect_event_cb, LV_EVENT_CLICKED, NULL);

    // Teclado genérico para a Aba 1
    lv_obj_t * kb_config = lv_keyboard_create(scr_ssh_config);
    lv_obj_add_flag(kb_config, LV_OBJ_FLAG_HIDDEN);
    
    auto kb_focus_cb = [](lv_event_t * e) {
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_t * ta = (lv_obj_t *)lv_event_get_target(e);
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    };
    auto kb_defocus_cb = [](lv_event_t * e) {
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    };

    lv_obj_add_event_cb(ta_host, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(ta_user, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(ta_pass, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(ta_port, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(tab_new, kb_defocus_cb, LV_EVENT_CLICKED, kb_config); // Tocar fora esconde

    // ------------------------------------------
    // ABA 2: SALVOS (LISTA)
    // ------------------------------------------
    lv_obj_t * list_saved = lv_list_create(tab_saved);
    lv_obj_set_size(list_saved, 360, 320);
    lv_obj_set_style_bg_color(list_saved, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_saved, 0, 0);

    lv_obj_t * t1 = lv_list_add_button(list_saved, LV_SYMBOL_DIRECTORY, "root @ 192.168.1.100 (Proxmox)");
    lv_obj_t * t2 = lv_list_add_button(list_saved, LV_SYMBOL_DIRECTORY, "ubuntu @ 10.0.0.50 (VPS)");
    lv_obj_set_style_bg_color(t1, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(t1, lv_color_white(), 0);
    lv_obj_set_style_bg_color(t2, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(t2, lv_color_white(), 0);
    // Simula clique no perfil conectando direto
    lv_obj_add_event_cb(t1, btn_connect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t2, btn_connect_event_cb, LV_EVENT_CLICKED, NULL);
}

// ==========================================
// EVENTOS WI-FI INTELIGENTE
// ==========================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_config_t saved_config = {};
        esp_wifi_get_config(WIFI_IF_STA, &saved_config);
        
        if (strlen((char*)saved_config.sta.ssid) > 0) {
            ESP_LOGI(TAG, "Tentando auto-conectar a %s...", saved_config.sta.ssid);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Sem credenciais. Escaneando redes...");
            start_wifi_scan();
            lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Falha de Conexão. Indo para Lista...");
        start_wifi_scan();
        lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 15) ap_count = 15;
        
        wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_info && esp_wifi_scan_get_ap_records(&ap_count, ap_info) == ESP_OK) {
            if (bsp_display_lock(pdMS_TO_TICKS(100))) {
                lv_obj_clean(list_wifi); 
                for (int i = 0; i < ap_count; i++) {
                    char ssid_str[33];
                    strncpy(ssid_str, (char *)ap_info[i].ssid, 32);
                    ssid_str[32] = '\0';
                    if (strlen(ssid_str) == 0) continue;

                    char list_item_text[64];
                    snprintf(list_item_text, sizeof(list_item_text), "%s (%d dBm)", ssid_str, ap_info[i].rssi);

                    lv_obj_t * btn = lv_list_add_button(list_wifi, LV_SYMBOL_WIFI, list_item_text);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
                    lv_obj_set_style_text_color(btn, lv_color_white(), 0);
                    lv_obj_add_event_cb(btn, wifi_list_item_click_cb, LV_EVENT_CLICKED, NULL);
                }
                bsp_display_unlock();
            }
        }
        if(ap_info) free(ap_info);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado! IP Local: " IPSTR, IP2STR(&event->ip_info.ip));
        
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            // Atualiza a Label verde no topo da tela de configuração
            lv_label_set_text_fmt(lbl_local_ip, "#AAAAAA IP Local:# #00FF00 " IPSTR " #", IP2STR(&event->ip_info.ip));
            lv_scr_load_anim(scr_ssh_config, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
            bsp_display_unlock();
        }
    }
}

static void wifi_init_client(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_FLASH); 
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

// ==========================================
// FUNÇÃO PRINCIPAL
// ==========================================
extern "C" void app_main(void) {
    clear_i2c_bus();
    esp_ota_mark_app_valid_cancel_rollback();

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 1. DISPLAY & SPLASH SCREEN
    bsp_display_start();
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        show_splash_screen("0.2-alpha");
        bsp_display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(50)); 
    bsp_display_brightness_set(80);

    // 2. CONSTRÓI AS TELAS EM SEGUNDO PLANO
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        build_wifi_ui();
        build_ssh_config_ui();
        build_terminal_ui();
        bsp_display_unlock();
    }

    // 3. INICIA HARDWARE & CONEXÃO (Wi-Fi Inteligente assume o controle visual)
    SdUsbManager::get_instance().init_local_storage();
    wifi_init_client(); 

    // Loop vigiando o botão físico
    while(1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) return_to_factory();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}