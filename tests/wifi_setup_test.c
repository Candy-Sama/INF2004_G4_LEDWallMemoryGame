#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/network/wifi_setup.c"

static const char *input;
static size_t position;
static bool usb = true;
static unsigned connections;
bool stdio_usb_connected(void) { return usb; }
int getchar_timeout_us(uint32_t timeout) {
    (void)timeout;
    assert(input[position] != '\0'); // Catch an unexpected extra prompt/hang.
    return (unsigned char)input[position++];
}
void sleep_ms(uint32_t duration) { (void)duration; assert(usb); }
bool wifi_init_and_connect(const char *ssid, const char *password) {
    ++connections;
    if (connections == 1) {
        assert(strcmp(ssid, "First network") == 0);
        assert(strcmp(password, "HiddenFirst123") == 0);
        return false;
    }
    assert(connections == 2);
    assert(strcmp(ssid, "Second network") == 0);
    assert(strcmp(password, "HiddenSecond456") == 0);
    return true;
}
static void feed(const char *text) { input = text; position = 0; }
int main(void) {
    char line[8] = {0};
    feed("ab\bcd\r\nnext\nlast\r");
    assert(read_line(line, sizeof(line), false) && strcmp(line, "acd") == 0);
    assert(read_line(line, sizeof(line), false) && strcmp(line, "next") == 0);
    assert(read_line(line, sizeof(line), false) && strcmp(line, "last") == 0);
    feed("12345678\nokay\n");
    assert(!read_line(line, sizeof(line), false));
    assert(read_line(line, sizeof(line), false) && strcmp(line, "okay") == 0);
    feed("  hi  \n");
    assert(read_line(line, sizeof(line), false) && strcmp(line, "  hi  ") == 0);
    feed("bad\tinput\n");
    assert(!read_line(line, sizeof(line), false));
    usb = false;
    memset(line, 's', sizeof(line));
    assert(!read_line(line, sizeof(line), false));
    for (size_t i = 0; i < sizeof(line); ++i) assert(line[i] == 0);
    usb = true;
    // Empty SSID and short password cause reprompt; connection failure retries.
    feed("\nShort network\nshort\nFirst network\r\nHiddenFirst123\r\nSecond network\nHiddenSecond456\n");
    wifi_setup_prompt();
    assert(connections == 2);
    assert(input[position] == '\0');
    puts("Serial Wi-Fi setup tests passed.");
}
