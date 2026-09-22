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
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "libssh2.h"

// ==========================================
// VARIÁVEIS GLOBAIS DO SSH
// ==========================================
static int ssh_sock = -1;
static LIBSSH2_SESSION *ssh_session = NULL;
static LIBSSH2_CHANNEL *ssh_channel = NULL;
static bool ssh_connected = false;
static TaskHandle_t ssh_rx_task_handle = NULL;

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

static lv_obj_t * tv_ssh = NULL; // TabView global para redimensionamento
static lv_obj_t * list_wifi = NULL;
static lv_obj_t * ta_wifi_pass = NULL;
static lv_obj_t * label_wifi_title = NULL;
static char current_ssid[33] = {0};

static lv_obj_t * lbl_local_ip = NULL;
static lv_obj_t * ta_host = NULL;
static lv_obj_t * ta_user = NULL;
static lv_obj_t * ta_pass = NULL;
static lv_obj_t * switch_auth = NULL;
static lv_obj_t * ta_port = NULL;

static lv_obj_t * ta_log = NULL;
static lv_obj_t * ta_input = NULL;
static lv_obj_t * kb_terminal = NULL;

// ==========================================
// ESTILOS GLOBAIS (TEMA ESCURO)
// ==========================================
static void style_dark_ta(lv_obj_t * ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    // Cursor luminoso idêntico ao app_notes
    lv_obj_set_style_border_width(ta, 2, (lv_style_selector_t)LV_PART_CURSOR | (lv_style_selector_t)LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, (lv_style_selector_t)LV_PART_CURSOR | (lv_style_selector_t)LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_white(), (lv_style_selector_t)LV_PART_CURSOR | (lv_style_selector_t)LV_STATE_FOCUSED);
}

static void style_dark_kb(lv_obj_t * kb) {
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x111111), LV_PART_MAIN); 
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN); 
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x333333), LV_PART_ITEMS); 
    lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS); 
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS); 
    // Teclas de controle escuras (Enter, Shift, Backspace)
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x444444), (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED); 
    lv_obj_set_style_text_color(kb, lv_color_white(), (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED); 
    // Margens anti-corte do visor circular
    lv_obj_set_style_pad_left(kb, 15, 0);
    lv_obj_set_style_pad_right(kb, 15, 0);
    lv_obj_set_style_pad_bottom(kb, 15, 0);
}

// ==========================================
// FUNÇÕES DE HARDWARE E SISTEMA
// ==========================================
static void return_to_factory() {
    ESP_LOGI(TAG, "Retornando ao Factory Firmware...");
    if (lvgl_port_lock(portMAX_DELAY)) {
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
    lv_obj_set_scrollbar_mode(scr_splash, LV_SCROLLBAR_MODE_OFF);

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
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        esp_wifi_disconnect(); 
    }
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&scan_config, false);
}

static void wifi_list_item_click_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0); 
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
    scr_wifi_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi_list, lv_color_black(), 0);
    
    lv_obj_t * header = lv_label_create(scr_wifi_list);
    lv_label_set_text(header, "Redes Wi-Fi"); 
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 30); 

    list_wifi = lv_obj_create(scr_wifi_list);
    lv_obj_set_size(list_wifi, 310, 380); 
    lv_obj_align(list_wifi, LV_ALIGN_TOP_MID, 0, 70); 
    lv_obj_set_style_bg_color(list_wifi, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_wifi, 0, 0);
    lv_obj_set_flex_flow(list_wifi, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * txt = lv_label_create(list_wifi);
    lv_label_set_text(txt, "Buscando...");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);

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
    style_dark_ta(ta_wifi_pass);

    lv_obj_t * kb = lv_keyboard_create(scr_password);
    lv_keyboard_set_textarea(kb, ta_wifi_pass);
    style_dark_kb(kb);
    
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
// MOTOR SSH (CORE 1 - FREE RTOS)
// ==========================================
static void ssh_disconnect() {
    ssh_connected = false;
    if (ssh_rx_task_handle) {
        vTaskDelete(ssh_rx_task_handle);
        ssh_rx_task_handle = NULL;
    }
    if (ssh_channel) {
        libssh2_channel_send_eof(ssh_channel);
        libssh2_channel_close(ssh_channel);
        libssh2_channel_free(ssh_channel);
        ssh_channel = NULL;
    }
    if (ssh_session) {
        libssh2_session_disconnect(ssh_session, "Saindo do SSHWatch");
        libssh2_session_free(ssh_session);
        ssh_session = NULL;
    }
    if (ssh_sock != -1) {
        close(ssh_sock);
        ssh_sock = -1;
    }
}

// Tarefa que fica vigiando se o Servidor mandou letras novas na tela
static void ssh_rx_task(void *pvParameters) {
    char buffer[256];
    while (ssh_connected && ssh_channel) {
        ssize_t rc = libssh2_channel_read(ssh_channel, buffer, sizeof(buffer) - 1);
        if (rc > 0) {
            buffer[rc] = '\0';
            if (bsp_display_lock(portMAX_DELAY)) {
                // Impede que o LVGL exploda a RAM limitando a 2000 letras na tela
                if (strlen(lv_textarea_get_text(ta_log)) > 2000) {
                    lv_textarea_set_text(ta_log, ""); 
                }
                lv_textarea_add_text(ta_log, buffer);
                bsp_display_unlock();
            }
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Sem mensagens novas, espera um pouco
        } else if (rc < 0) {
            ESP_LOGE(TAG, "Erro de Leitura SSH: %d", (int)rc);
            break;
        } else if (libssh2_channel_eof(ssh_channel)) {
            break; // Servidor encerrou a conexão
        }
    }
    
    ssh_disconnect();
    if (bsp_display_lock(portMAX_DELAY)) {
        lv_textarea_add_text(ta_log, "\n[Conexão Encerrada pelo Servidor]\n");
        bsp_display_unlock();
    }
    ssh_rx_task_handle = NULL;
    vTaskDelete(NULL);
}

// Tarefa Pesada de Conexão
static void ssh_connect_task(void *pvParameters) {
    char host[64], user[64], pass[64], port_str[8];
    bool use_key = false;

    // 1. Copia os dados da interface com segurança
    if (bsp_display_lock(portMAX_DELAY)) {
        strncpy(host, lv_textarea_get_text(ta_host), 63);
        strncpy(user, lv_textarea_get_text(ta_user), 63);
        strncpy(pass, lv_textarea_get_text(ta_pass), 63);
        strncpy(port_str, lv_textarea_get_text(ta_port), 7);
        use_key = lv_obj_has_state(switch_auth, LV_STATE_CHECKED);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "Conectando via TCP a %s:%s...", host, port_str);
    
    struct sockaddr_in sin;
    struct hostent *he = gethostbyname(host);
    if (!he) goto connect_error;

    ssh_sock = socket(AF_INET, SOCK_STREAM, 0);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(atoi(port_str));
    sin.sin_addr = *(struct in_addr *)he->h_addr_list[0];

    if (connect(ssh_sock, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) != 0) {
        ESP_LOGE(TAG, "Falha na conexão TCP.");
        goto connect_error;
    }

    ESP_LOGI(TAG, "TCP OK! Iniciando Handshake SSH...");
    ssh_session = libssh2_session_init();
    libssh2_session_set_blocking(ssh_session, 1);
    
    if (libssh2_session_handshake(ssh_session, ssh_sock)) {
        ESP_LOGE(TAG, "Falha no Handshake Criptográfico.");
        goto connect_error;
    }

    ESP_LOGI(TAG, "Handshake OK! Autenticando...");
    if (use_key) {
        // Busca a chave na pasta que você pediu!
        if (libssh2_userauth_publickey_fromfile(ssh_session, user, "/sdcard/SSH/id_rsa.pub", "/sdcard/SSH/id_rsa", pass)) {
            ESP_LOGE(TAG, "Falha ao ler chave no SD ou Chave Recusada.");
            goto connect_error;
        }
    } else {
        if (libssh2_userauth_password(ssh_session, user, pass)) {
            ESP_LOGE(TAG, "Senha Recusada pelo Servidor.");
            goto connect_error;
        }
    }

    ESP_LOGI(TAG, "Autenticado! Abrindo Terminal (PTY)...");
    ssh_channel = libssh2_channel_open_session(ssh_session);
    if (!ssh_channel) goto connect_error;

    // Pede um emulador de terminal Linux padrão
    if (libssh2_channel_request_pty(ssh_channel, "linux")) goto connect_error;
    if (libssh2_channel_shell(ssh_channel)) goto connect_error;

    // Configura para Modo Não-Bloqueante (Para podermos enviar e receber ao mesmo tempo)
    libssh2_session_set_blocking(ssh_session, 0);
    ssh_connected = true;

    // SUCESSO! Muda a tela para o Terminal e apaga o Spinner
    if (bsp_display_lock(portMAX_DELAY)) {
        extern lv_obj_t * conn_spinner;
        if (conn_spinner) { lv_obj_delete(conn_spinner); conn_spinner = NULL; }
        
        lv_textarea_set_text(ta_log, ""); // Limpa o texto padrão
        lv_scr_load_anim(scr_terminal, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
        bsp_display_unlock();
    }

    // Inicia o leitor de respostas do servidor
    xTaskCreatePinnedToCore(ssh_rx_task, "ssh_rx", 8192, NULL, 5, &ssh_rx_task_handle, 1);
    vTaskDelete(NULL);
    return;

connect_error:
    ssh_disconnect();
    if (bsp_display_lock(portMAX_DELAY)) {
        extern lv_obj_t * conn_spinner;
        if (conn_spinner) { lv_obj_delete(conn_spinner); conn_spinner = NULL; }
        lv_label_set_text(lbl_local_ip, "#FF0000 Erro ao Conectar SSH!#");
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

// ==========================================
// INTERFACE: TERMINAL DE COMANDO
// ==========================================
static void terminal_kb_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_READY) {
        const char * cmd = lv_textarea_get_text(ta_input);
        
        // Se estiver conectado, envia o comando com o botão ENTER real!
        if (ssh_connected && ssh_channel) {
            char full_cmd[256];
            snprintf(full_cmd, sizeof(full_cmd), "%s\n", cmd);
            
            // Pausa temporária no RX para escrever
            libssh2_session_set_blocking(ssh_session, 1);
            libssh2_channel_write(ssh_channel, full_cmd, strlen(full_cmd));
            libssh2_session_set_blocking(ssh_session, 0);
        }
        
        lv_textarea_set_text(ta_input, ""); 
    }
}

static void build_terminal_ui() {
    scr_terminal = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_terminal, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(scr_terminal, LV_SCROLLBAR_MODE_OFF);

    ta_log = lv_textarea_create(scr_terminal);
    lv_obj_set_size(ta_log, 390, 420); 
    lv_obj_align(ta_log, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(ta_log, lv_color_black(), 0);
    lv_obj_set_style_text_color(ta_log, lv_color_hex(0x00FF00), 0); 
    lv_obj_set_style_border_width(ta_log, 0, 0);
    lv_textarea_set_cursor_click_pos(ta_log, false);
    lv_textarea_set_text(ta_log, "Conectado com sucesso!\nSSHWatch OS v1.0\nType commands below.\n");

    ta_input = lv_textarea_create(scr_terminal);
    lv_obj_set_size(ta_input, 390, 45);
    lv_obj_align(ta_input, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_textarea_set_one_line(ta_input, true);
    lv_textarea_set_placeholder_text(ta_input, "> Digite um comando...");
    style_dark_ta(ta_input);

    kb_terminal = lv_keyboard_create(scr_terminal);
    lv_keyboard_set_textarea(kb_terminal, ta_input);
    lv_obj_set_hidden(kb_terminal, true); 
    style_dark_kb(kb_terminal);
    lv_obj_add_event_cb(kb_terminal, terminal_kb_event_cb, LV_EVENT_READY, NULL);

    lv_obj_add_event_cb(ta_input, [](lv_event_t * e) {
        lv_obj_set_hidden(kb_terminal, false);
        lv_obj_set_height(ta_log, 160);  
        lv_obj_align(ta_input, LV_ALIGN_TOP_MID, 0, 180); 
    }, LV_EVENT_FOCUSED, NULL);

    lv_obj_add_event_cb(ta_log, [](lv_event_t * e) {
        lv_obj_set_hidden(kb_terminal, true); 
        lv_obj_set_height(ta_log, 420); 
        lv_obj_align(ta_input, LV_ALIGN_BOTTOM_MID, 0, -10); 
        lv_obj_remove_state(ta_input, LV_STATE_FOCUSED); 
    }, LV_EVENT_CLICKED, NULL);

    // Botão Sair Compacto (Transparente, apenas com o ícone flutuando)
    lv_obj_t * btn_exit = lv_button_create(scr_terminal);
    lv_obj_set_size(btn_exit, 45, 45);
    lv_obj_align(btn_exit, LV_ALIGN_TOP_RIGHT, -25, 25);
    
    // Remove o fundo, a borda e a sombra (deixa invisível)
    lv_obj_set_style_bg_opa(btn_exit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_exit, 0, 0);
    lv_obj_set_style_shadow_width(btn_exit, 0, 0);
    
    lv_obj_t * lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(lbl_exit, &lv_font_montserrat_20, 0);
    
    // Pinta EXCLUSIVAMENTE o símbolo com um vermelho bem vivo
    lv_obj_set_style_text_color(lbl_exit, lv_color_hex(0xFF0000), 0);
    lv_obj_center(lbl_exit);
    
    // Ação: Retornar para a tela de configurações do SSH (scr_ssh_config)
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e){ 
        lv_obj_set_hidden(kb_terminal, true); 
        ssh_disconnect(); // CORTA A CONEXÃO DE VERDADE AQUI
        lv_scr_load_anim(scr_ssh_config, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }, LV_EVENT_CLICKED, NULL);

    // =========================================
    // BOTÃO TROCAR WI-FI
    // =========================================
    lv_obj_t * btn_wifi = lv_button_create(scr_ssh_config);
    lv_obj_set_size(btn_wifi, 40, 40);
    lv_obj_align(btn_wifi, LV_ALIGN_TOP_RIGHT, -60, 5); // Posicionado ao lado do botão Sair
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x222222), 0); // Fundo escuro
    lv_obj_set_style_radius(btn_wifi, LV_RADIUS_CIRCLE, 0); // Formato redondo
    
    lv_obj_t * lbl_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi, lv_color_white(), 0); 
    lv_obj_center(lbl_wifi);
    
    lv_obj_add_event_cb(btn_wifi, [](lv_event_t *e){ 
        // 1. Limpa as credenciais atuais na memória para evitar auto-reconexão imediata
        wifi_config_t empty_config = {};
        esp_wifi_set_config(WIFI_IF_STA, &empty_config);
        
        // 2. Corta a conexão do rádio
        // (A transição de tela e o novo escaneamento vão ser disparados 
        // automaticamente pelo nosso wifi_event_handler!)
        esp_wifi_disconnect(); 
    }, LV_EVENT_CLICKED, NULL);
}

// ==========================================
// INTERFACE: SSH CONFIG (TAB VIEW)
// ==========================================
lv_obj_t * conn_spinner = NULL;

static void btn_connect_event_cb(lv_event_t * e) {
    if (conn_spinner) lv_obj_delete(conn_spinner); 
    
    conn_spinner = lv_spinner_create(scr_ssh_config);
    lv_obj_set_size(conn_spinner, 100, 100);
    lv_obj_center(conn_spinner);
    lv_spinner_set_anim_params(conn_spinner, 1000, 60);

    // Inicia o motor SSH em segundo plano com uma Pilha Gigante (Criptografia)
    xTaskCreatePinnedToCore(ssh_connect_task, "ssh_conn", 24000, NULL, 5, NULL, 1);
}

static void build_ssh_config_ui() {
    scr_ssh_config = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ssh_config, lv_color_black(), 0);

    lbl_local_ip = lv_label_create(scr_ssh_config);
    lv_label_set_text(lbl_local_ip, "IP Local: Aguardando...");
    lv_obj_set_style_text_color(lbl_local_ip, lv_color_hex(0x00FF00), 0); 
    lv_label_set_long_mode(lbl_local_ip, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(lbl_local_ip, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_local_ip, LV_ALIGN_TOP_LEFT, 65, 10);

    tv_ssh = lv_tabview_create(scr_ssh_config);
    lv_tabview_set_tab_bar_position(tv_ssh, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv_ssh, 50);
    lv_obj_set_size(tv_ssh, 410, 440);
    lv_obj_align(tv_ssh, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(tv_ssh, lv_color_black(), 0);

    // Colorir Abas do TabView (Tema Escuro)
    lv_obj_t * tab_bar = lv_tabview_get_tab_bar(tv_ssh);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(tab_bar, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_color(tab_bar, lv_color_white(), (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);

    lv_obj_t * tab_new = lv_tabview_add_tab(tv_ssh, "Nova Conexao");
    lv_obj_t * tab_saved = lv_tabview_add_tab(tv_ssh, "Salvos");

    lv_obj_set_flex_flow(tab_new, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab_new, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_new, 10, 0);
    lv_obj_set_style_pad_row(tab_new, 15, 0);

    ta_host = lv_textarea_create(tab_new);
    lv_textarea_set_one_line(ta_host, true);
    lv_textarea_set_placeholder_text(ta_host, "IP ou Hostname");
    lv_obj_set_width(ta_host, 340);
    style_dark_ta(ta_host);

    ta_user = lv_textarea_create(tab_new);
    lv_textarea_set_one_line(ta_user, true);
    lv_textarea_set_placeholder_text(ta_user, "Usuario (ex: root)");
    lv_obj_set_width(ta_user, 340);
    style_dark_ta(ta_user);

    ta_pass = lv_textarea_create(tab_new);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_textarea_set_placeholder_text(ta_pass, "Senha");
    lv_obj_set_width(ta_pass, 340);
    style_dark_ta(ta_pass);

    lv_obj_t * row_opt = lv_obj_create(tab_new);
    lv_obj_set_size(row_opt, 340, 50);
    lv_obj_set_style_bg_opa(row_opt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_opt, 0, 0);
    lv_obj_set_scrollbar_mode(row_opt, LV_SCROLLBAR_MODE_OFF); // <-- Corrige a barra de rolagem flutuando
    lv_obj_set_flex_flow(row_opt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_opt, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    switch_auth = lv_switch_create(row_opt);
    
    // Adiciona a lógica para desabilitar o campo de senha quando usar chave
    lv_obj_add_event_cb(switch_auth, [](lv_event_t * e) {
        lv_obj_t * sw = (lv_obj_t *)lv_event_get_target(e);
        if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
            // Usando Chave SD: Bloqueia e deixa o campo de senha "apagado"
            lv_obj_add_state(ta_pass, LV_STATE_DISABLED);
            lv_obj_set_style_opa(ta_pass, LV_OPA_30, 0); 
            lv_textarea_set_text(ta_pass, ""); // Opcional: Limpa se tiver algo
        } else {
            // Usando Senha: Acende e destrava o campo
            lv_obj_remove_state(ta_pass, LV_STATE_DISABLED);
            lv_obj_set_style_opa(ta_pass, LV_OPA_COVER, 0);
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t * lbl_sw = lv_label_create(row_opt);
    lv_label_set_text(lbl_sw, "Usar Chave SD");
    lv_obj_set_style_text_color(lbl_sw, lv_color_white(), 0);

    ta_port = lv_textarea_create(row_opt);
    lv_textarea_set_one_line(ta_port, true);
    lv_textarea_set_text(ta_port, "22");
    lv_obj_set_width(ta_port, 80);
    style_dark_ta(ta_port);

    lv_obj_t * btn_connect = lv_button_create(tab_new);
    lv_obj_set_size(btn_connect, 200, 60);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0x007BFF), 0);
    lv_obj_set_style_radius(btn_connect, 30, 0);
    lv_obj_t * lbl_conn = lv_label_create(btn_connect);
    lv_label_set_text(lbl_conn, "Conectar");
    lv_obj_set_style_text_font(lbl_conn, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_conn);
    lv_obj_add_event_cb(btn_connect, btn_connect_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * kb_config = lv_keyboard_create(scr_ssh_config);
    lv_obj_set_hidden(kb_config, true); 
    style_dark_kb(kb_config);
    
    auto kb_focus_cb = [](lv_event_t * e) {
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_t * ta = (lv_obj_t *)lv_event_get_target(e);
        lv_keyboard_set_textarea(kb, ta);
        lv_obj_set_hidden(kb, false);
        
        if (tv_ssh) {
            int y_offset = 0;
            // Verifica quem chamou o evento e desliza a tela proporcionalmente
            if (ta == ta_host) y_offset = -10;
            else if (ta == ta_user) y_offset = -60;
            else if (ta == ta_pass) y_offset = -110;
            else if (ta == ta_port) y_offset = -150;
            
            lv_obj_set_style_translate_y(tv_ssh, y_offset, 0); 
        }
    };
    
    auto kb_hide_cb = [](lv_event_t * e) {
        lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_set_hidden(kb, true);
        
        // Desliza a tela de volta para o lugar original quando o teclado fecha
        if (tv_ssh) {
            lv_obj_set_style_translate_y(tv_ssh, 0, 0); 
        }
    };

    lv_obj_add_event_cb(ta_host, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(ta_user, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(ta_pass, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    lv_obj_add_event_cb(ta_port, kb_focus_cb, LV_EVENT_FOCUSED, kb_config);
    
    // Oculta ao tocar fora ou nos controles do teclado
    lv_obj_add_event_cb(tab_new, kb_hide_cb, LV_EVENT_CLICKED, kb_config); 
    lv_obj_add_event_cb(kb_config, kb_hide_cb, LV_EVENT_READY, kb_config);
    lv_obj_add_event_cb(kb_config, kb_hide_cb, LV_EVENT_CANCEL, kb_config);

    lv_obj_t * list_saved = lv_obj_create(tab_saved);
    lv_obj_set_size(list_saved, 360, 320);
    lv_obj_set_style_bg_color(list_saved, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_saved, 0, 0);
    lv_obj_set_flex_flow(list_saved, LV_FLEX_FLOW_COLUMN);

    auto add_saved_btn = [](lv_obj_t * parent, const char * txt) {
        lv_obj_t * btn = lv_button_create(parent);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_t * lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%s %s", LV_SYMBOL_DIRECTORY, txt);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        return btn;
    };

    lv_obj_t * t1 = add_saved_btn(list_saved, "root @ 192.168.1.100");
    lv_obj_add_event_cb(t1, btn_connect_event_cb, LV_EVENT_CLICKED, NULL);
}

// ==========================================
// EVENTOS WI-FI INTELIGENTE
// ==========================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_config_t saved_config = {};
        esp_wifi_get_config(WIFI_IF_STA, &saved_config);
        
        if (strlen((char*)saved_config.sta.ssid) > 0) {
            esp_wifi_connect();
        } else {
            start_wifi_scan();
            // CADEADO DE SEGURANÇA ADICIONADO
            if (bsp_display_lock(portMAX_DELAY)) {
                lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
                bsp_display_unlock();
            }
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        start_wifi_scan();
        // CADEADO DE SEGURANÇA ADICIONADO
        if (bsp_display_lock(portMAX_DELAY)) {
            // Só faz a animação se já não estivermos na lista (evita glitch visual)
            if (lv_screen_active() != scr_wifi_list) {
                lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
            }
            bsp_display_unlock();
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 15) ap_count = 15;
        
        wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_info && esp_wifi_scan_get_ap_records(&ap_count, ap_info) == ESP_OK) {
            if (bsp_display_lock(portMAX_DELAY)) {
                lv_obj_clean(list_wifi); 
                for (int i = 0; i < ap_count; i++) {
                    char ssid_str[33];
                    strncpy(ssid_str, (char *)ap_info[i].ssid, 32);
                    ssid_str[32] = '\0';
                    if (strlen(ssid_str) == 0) continue;

                    char list_item_text[64];
                    snprintf(list_item_text, sizeof(list_item_text), "%s (%d dBm)", ssid_str, ap_info[i].rssi);

                    lv_obj_t * btn = lv_button_create(list_wifi);
                    lv_obj_set_width(btn, lv_pct(100));
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
                    lv_obj_t * lbl = lv_label_create(btn);
                    lv_label_set_text(lbl, list_item_text);
                    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
                    lv_obj_center(lbl);
                    lv_obj_add_event_cb(btn, wifi_list_item_click_cb, LV_EVENT_CLICKED, NULL);
                }
                bsp_display_unlock();
            }
        }
        if(ap_info) free(ap_info);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        if (bsp_display_lock(portMAX_DELAY)) {
            lv_label_set_text_fmt(lbl_local_ip, "IP Local: " IPSTR, IP2STR(&event->ip_info.ip));
            if (lv_screen_active() != scr_ssh_config) {
                lv_scr_load_anim(scr_ssh_config, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
            }
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
// FUNÇÃO PRINCIPAL (Sequência de Boot Segura)
// ==========================================
extern "C" void app_main(void) {
    libssh2_init(0);
    clear_i2c_bus();
    
    // Proteção de Hardware I2C (Evita colapso do AXP2101)
    gpio_set_pull_mode(GPIO_NUM_14, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_NUM_15, GPIO_PULLUP_ONLY);

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

    bsp_display_start();
    if (bsp_display_lock(portMAX_DELAY)) {
        show_splash_screen("0.3-alpha");
        bsp_display_unlock();
    }
    
    vTaskDelay(pdMS_TO_TICKS(200)); 
    bsp_display_brightness_set(80);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (bsp_display_lock(portMAX_DELAY)) {
        build_wifi_ui();
        build_ssh_config_ui();
        build_terminal_ui();
        bsp_display_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    SdUsbManager::get_instance().init_local_storage();
    wifi_init_client(); 

    while(1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) return_to_factory();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}