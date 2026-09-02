#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define PORT 5001

#define MAX_EVENTS 32
#define MAX_CLIENTS 100
#define MAX_USERS 1000

#define USERNAME_SIZE 32
#define MESSAGE_SIZE 1024
#define INPUT_BUFFER_SIZE 4096

#define USERS_FILE "users.db"
#define MESSAGES_FILE "messages.db"

enum ClientMode {
    MODE_LOGIN,
    MODE_MENU,
    MODE_CHAT,
    MODE_ADMIN
};

struct User {
    int id;
    char username[USERNAME_SIZE];

    int active;
    int is_admin;
};

struct Client {
    int fd;

    int active;
    int logged_in;

    int user_id;

    enum ClientMode mode;

    int chat_partner_id;

    char input_buffer[INPUT_BUFFER_SIZE];
    size_t input_len;

    int menu_contact_ids[MAX_USERS];
    int menu_contact_count;
};

struct HistoryMessage {
    long timestamp;

    int sender_id;
    int receiver_id;

    char message[MESSAGE_SIZE];
};

static struct User users[MAX_USERS];
static int user_count = 0;

static struct Client clients[MAX_CLIENTS];

static int epoll_fd;


/*
 * ----------------------------------------------------------
 * TCP SEND
 * ----------------------------------------------------------
 */

static int send_all(
    int fd,
    const char *buffer,
    size_t length
)
{
    size_t sent = 0;

    while (sent < length) {

        ssize_t n = send(
            fd,
            buffer + sent,
            length - sent,
            0
        );

        if (n < 0) {

            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}


static int send_text(
    int fd,
    const char *text
)
{
    return send_all(
        fd,
        text,
        strlen(text)
    );
}


static int send_format(
    int fd,
    const char *format,
    ...
)
{
    char buffer[4096];

    va_list args;

    va_start(args, format);

    vsnprintf(
        buffer,
        sizeof(buffer),
        format,
        args
    );

    va_end(args);

    return send_text(fd, buffer);
}


/*
 * ----------------------------------------------------------
 * USER FUNCTIONS
 * ----------------------------------------------------------
 */

static int find_user_index_by_id(int id)
{
    for (int i = 0; i < user_count; i++) {

        if (users[i].id == id) {
            return i;
        }
    }

    return -1;
}


static int find_user_index_by_name(
    const char *username
)
{
    for (int i = 0; i < user_count; i++) {

        if (
            strcmp(
                users[i].username,
                username
            ) == 0
        ) {
            return i;
        }
    }

    return -1;
}


static int get_max_user_id(void)
{
    int max_id = 0;

    for (int i = 0; i < user_count; i++) {

        if (users[i].id > max_id) {
            max_id = users[i].id;
        }
    }

    return max_id;
}


/*
 * users.db:
 *
 * id|username|active|is_admin
 *
 * 1|ahmet|1|1
 * 2|mehmet|1|0
 * 3|ali|0|0
 */

static int save_users(void)
{
    FILE *file = fopen(
        USERS_FILE ".tmp",
        "w"
    );

    if (file == NULL) {
        perror("fopen users tmp");
        return -1;
    }

    for (int i = 0; i < user_count; i++) {

        fprintf(
            file,
            "%d|%s|%d|%d\n",
            users[i].id,
            users[i].username,
            users[i].active,
            users[i].is_admin
        );
    }

    fclose(file);

    if (
        rename(
            USERS_FILE ".tmp",
            USERS_FILE
        ) < 0
    ) {

        perror("rename users.db");

        return -1;
    }

    return 0;
}


static int load_users(void)
{
    FILE *file = fopen(
        USERS_FILE,
        "a+"
    );

    if (file == NULL) {
        perror("fopen users.db");
        return -1;
    }

    rewind(file);

    user_count = 0;

    char line[256];

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        if (user_count >= MAX_USERS) {
            break;
        }

        struct User user;

        memset(
            &user,
            0,
            sizeof(user)
        );

        /*
         * Yeni format:
         *
         * id|username|active|admin
         */
        int count = sscanf(
            line,
            "%d|%31[^|]|%d|%d",
            &user.id,
            user.username,
            &user.active,
            &user.is_admin
        );

        if (count == 4) {

            users[user_count++] = user;

            continue;
        }

        /*
         * Eski projemizdeki:
         *
         * id|username
         *
         * formatını da okuyabilsin.
         */
        memset(
            &user,
            0,
            sizeof(user)
        );

        count = sscanf(
            line,
            "%d|%31s",
            &user.id,
            user.username
        );

        if (count == 2) {

            user.active = 1;

            user.is_admin =
                strcmp(
                    user.username,
                    "ahmet"
                ) == 0;

            users[user_count++] = user;
        }
    }

    fclose(file);

    /*
     * Default admin:
     *
     * ahmet
     */
    int ahmet_index =
        find_user_index_by_name("ahmet");

    if (ahmet_index == -1) {

        if (user_count >= MAX_USERS) {
            return -1;
        }

        struct User *admin =
            &users[user_count++];

        admin->id =
            get_max_user_id() + 1;

        strcpy(
            admin->username,
            "ahmet"
        );

        admin->active = 1;
        admin->is_admin = 1;
    }
    else {

        /*
         * ahmet her zaman admin olarak
         * korunuyor.
         */
        users[ahmet_index].active = 1;
        users[ahmet_index].is_admin = 1;
    }

    /*
     * Eski users.db varsa yeni formata
     * dönüştürmüş oluyoruz.
     */
    return save_users();
}


/*
 * ----------------------------------------------------------
 * CLIENT FUNCTIONS
 * ----------------------------------------------------------
 */

static int find_client_by_user_id(
    int user_id
)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (
            clients[i].active &&
            clients[i].logged_in &&
            clients[i].user_id == user_id
        ) {
            return i;
        }
    }

    return -1;
}


static int find_free_client_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {

        if (!clients[i].active) {
            return i;
        }
    }

    return -1;
}


static void disconnect_client(
    int client_index
)
{
    if (
        client_index < 0 ||
        client_index >= MAX_CLIENTS
    ) {
        return;
    }

    if (!clients[client_index].active) {
        return;
    }

    int fd = clients[client_index].fd;

    epoll_ctl(
        epoll_fd,
        EPOLL_CTL_DEL,
        fd,
        NULL
    );

    close(fd);

    memset(
        &clients[client_index],
        0,
        sizeof(struct Client)
    );

    clients[client_index].fd = -1;
}


/*
 * ----------------------------------------------------------
 * MESSAGE DATABASE
 * ----------------------------------------------------------
 *
 * messages.db:
 *
 * timestamp|sender_id|receiver_id|message
 */

static void sanitize_message(
    char *message
)
{
    for (
        size_t i = 0;
        message[i] != '\0';
        i++
    ) {

        if (message[i] == '|') {
            message[i] = '/';
        }

        if (
            message[i] == '\r' ||
            message[i] == '\n'
        ) {
            message[i] = '\0';
            break;
        }
    }
}


static int append_message(
    int sender_id,
    int receiver_id,
    const char *text
)
{
    FILE *file = fopen(
        MESSAGES_FILE,
        "a"
    );

    if (file == NULL) {
        perror("messages.db");
        return -1;
    }

    char clean[MESSAGE_SIZE];

    strncpy(
        clean,
        text,
        sizeof(clean) - 1
    );

    clean[
        sizeof(clean) - 1
    ] = '\0';

    sanitize_message(clean);

    fprintf(
        file,
        "%ld|%d|%d|%s\n",
        (long)time(NULL),
        sender_id,
        receiver_id,
        clean
    );

    fclose(file);

    return 0;
}


/*
 * ----------------------------------------------------------
 * CHAT HISTORY
 * ----------------------------------------------------------
 */

static void show_last_messages(
    int client_index,
    int partner_id
)
{
    FILE *file = fopen(
        MESSAGES_FILE,
        "r"
    );

    if (file == NULL) {

        send_text(
            clients[client_index].fd,
            "\nNo previous messages.\n"
        );

        return;
    }

    struct HistoryMessage history[10];

    int history_count = 0;

    char line[1400];

    int my_id =
        clients[client_index].user_id;

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        struct HistoryMessage current;

        memset(
            &current,
            0,
            sizeof(current)
        );

        int parsed = sscanf(
            line,
            "%ld|%d|%d|%1023[^\n]",
            &current.timestamp,
            &current.sender_id,
            &current.receiver_id,
            current.message
        );

        if (parsed < 3) {
            continue;
        }

        int belongs_to_chat =

            (
                current.sender_id == my_id &&
                current.receiver_id == partner_id
            )

            ||

            (
                current.sender_id == partner_id &&
                current.receiver_id == my_id
            );

        if (!belongs_to_chat) {
            continue;
        }

        if (history_count < 10) {

            history[history_count++] =
                current;
        }
        else {

            memmove(
                &history[0],
                &history[1],
                sizeof(history[0]) * 9
            );

            history[9] = current;
        }
    }

    fclose(file);

    if (history_count == 0) {

        send_text(
            clients[client_index].fd,
            "\nNo previous messages.\n"
        );

        return;
    }

    send_text(
        clients[client_index].fd,
        "\n----- LAST 10 MESSAGES -----\n"
    );

    for (int i = 0; i < history_count; i++) {

        int sender_index =
            find_user_index_by_id(
                history[i].sender_id
            );

        const char *sender_name =
            "unknown";

        if (sender_index >= 0) {

            sender_name =
                users[sender_index].username;
        }

        char time_buffer[32];

        time_t value =
            (time_t)history[i].timestamp;

        struct tm tm_value;

        localtime_r(
            &value,
            &tm_value
        );

        strftime(
            time_buffer,
            sizeof(time_buffer),
            "%H:%M",
            &tm_value
        );

        send_format(
            clients[client_index].fd,
            "[%s] %s: %s\n",
            time_buffer,
            sender_name,
            history[i].message
        );
    }

    send_text(
        clients[client_index].fd,
        "----------------------------\n"
    );
}


/*
 * ----------------------------------------------------------
 * CONVERSATION LIST
 * ----------------------------------------------------------
 */

static void show_conversation_list(
    int client_index
)
{
    struct Client *client =
        &clients[client_index];

    client->menu_contact_count = 0;

    long last_time[MAX_USERS];

    memset(
        last_time,
        0,
        sizeof(last_time)
    );

    FILE *file = fopen(
        MESSAGES_FILE,
        "r"
    );

    if (file != NULL) {

        char line[1400];

        while (
            fgets(
                line,
                sizeof(line),
                file
            ) != NULL
        ) {

            long timestamp;
            int sender;
            int receiver;

            int parsed = sscanf(
                line,
                "%ld|%d|%d|",
                &timestamp,
                &sender,
                &receiver
            );

            if (parsed < 3) {
                continue;
            }

            int other_id = -1;

            if (sender == client->user_id) {
                other_id = receiver;
            }
            else if (
                receiver == client->user_id
            ) {
                other_id = sender;
            }

            if (other_id == -1) {
                continue;
            }

            int position = -1;

            for (
                int i = 0;
                i < client->menu_contact_count;
                i++
            ) {

                if (
                    client->menu_contact_ids[i]
                    == other_id
                ) {

                    position = i;
                    break;
                }
            }

            if (position == -1) {

                if (
                    client->menu_contact_count
                    >= MAX_USERS
                ) {
                    continue;
                }

                position =
                    client->menu_contact_count++;

                client->menu_contact_ids[position] =
                    other_id;
            }

            if (timestamp > last_time[position]) {
                last_time[position] = timestamp;
            }
        }

        fclose(file);
    }

    /*
     * Son konuşulan kişi yukarı gelsin.
     */
    for (
        int i = 0;
        i < client->menu_contact_count;
        i++
    ) {

        for (
            int j = i + 1;
            j < client->menu_contact_count;
            j++
        ) {

            if (last_time[j] > last_time[i]) {

                long temp_time =
                    last_time[i];

                last_time[i] =
                    last_time[j];

                last_time[j] =
                    temp_time;

                int temp_id =
                    client->menu_contact_ids[i];

                client->menu_contact_ids[i] =
                    client->menu_contact_ids[j];

                client->menu_contact_ids[j] =
                    temp_id;
            }
        }
    }

    send_text(
        client->fd,
        "\n========== CHATS ==========\n"
    );

    if (
        client->menu_contact_count == 0
    ) {

        send_text(
            client->fd,
            "No previous conversations.\n"
        );
    }

    for (
        int i = 0;
        i < client->menu_contact_count;
        i++
    ) {

        int user_index =
            find_user_index_by_id(
                client->menu_contact_ids[i]
            );

        if (user_index == -1) {
            continue;
        }

        const char *status;

        if (!users[user_index].active) {

            status = "removed";
        }
        else if (
            find_client_by_user_id(
                users[user_index].id
            ) >= 0
        ) {

            status = "online";
        }
        else {

            status = "offline";
        }

        send_format(
            client->fd,
            "%d. %s [%s]\n",
            i + 1,
            users[user_index].username,
            status
        );
    }

    send_text(
        client->fd,
        "\n"
        "Number       : open chat\n"
        "n <username> : new/open chat\n"
        "r            : refresh\n"
    );

    int my_index =
        find_user_index_by_id(
            client->user_id
        );

    if (
        my_index >= 0 &&
        users[my_index].is_admin
    ) {

        send_text(
            client->fd,
            "a            : admin page\n"
        );
    }

    send_text(
        client->fd,
        "q            : disconnect\n"
        "===========================\n"
        "> "
    );
}


/*
 * ----------------------------------------------------------
 * OPEN CHAT
 * ----------------------------------------------------------
 */

static void open_chat(
    int client_index,
    int partner_id
)
{
    int partner_index =
        find_user_index_by_id(
            partner_id
        );

    if (partner_index == -1) {

        send_text(
            clients[client_index].fd,
            "User not found.\n> "
        );

        return;
    }

    if (
        partner_id ==
        clients[client_index].user_id
    ) {

        send_text(
            clients[client_index].fd,
            "You cannot chat with yourself.\n> "
        );

        return;
    }

    clients[client_index].mode =
        MODE_CHAT;

    clients[client_index].chat_partner_id =
        partner_id;

    send_format(
        clients[client_index].fd,
        "\n===== CHAT: %s =====\n",
        users[partner_index].username
    );

    if (!users[partner_index].active) {

        send_text(
            clients[client_index].fd,
            "[This user has been removed. "
            "History is read-only.]\n"
        );
    }

    show_last_messages(
        client_index,
        partner_id
    );

    send_text(
        clients[client_index].fd,
        "Type q and press Enter to leave chat.\n"
        "> "
    );
}


/*
 * ----------------------------------------------------------
 * ADMIN PAGE
 * ----------------------------------------------------------
 */

static void show_admin_page(
    int client_index
)
{
    send_text(
        clients[client_index].fd,
        "\n========== ADMIN ==========\n"
    );

    for (
        int i = 0;
        i < user_count;
        i++
    ) {

        send_format(
            clients[client_index].fd,
            "%d. %s  ID=%d  [%s]%s\n",
            i + 1,
            users[i].username,
            users[i].id,
            users[i].active
                ? "active"
                : "removed",
            users[i].is_admin
                ? " [ADMIN]"
                : ""
        );
    }

    send_text(
        clients[client_index].fd,
        "\n"
        "d <number> : remove user\n"
        "r <number> : restore user\n"
        "q          : leave admin page\n"
        "===========================\n"
        "admin> "
    );
}


/*
 * ----------------------------------------------------------
 * LOGIN
 * ----------------------------------------------------------
 */

static int process_login(
    int client_index,
    char *line
)
{
    struct Client *client =
        &clients[client_index];

    sanitize_message(line);

    if (line[0] == '\0') {

        send_text(
            client->fd,
            "Username cannot be empty.\n"
            "Enter your username: "
        );

        return 1;
    }

    if (
        strlen(line) >= USERNAME_SIZE
    ) {

        send_text(
            client->fd,
            "Username is too long.\n"
            "Enter your username: "
        );

        return 1;
    }

    int user_index =
        find_user_index_by_name(line);

    /*
     * Existing username.
     */
    if (user_index >= 0) {

        /*
         * Removed users cannot login.
         *
         * Username nevertheless stays reserved.
         */
        if (!users[user_index].active) {

            send_text(
                client->fd,
                "This account has been removed "
                "and cannot currently be used.\n"
            );

            disconnect_client(
                client_index
            );

            return 0;
        }

        /*
         * Aynı hesabın ikinci defa login
         * olmasını şimdilik engelliyoruz.
         */
        if (
            find_client_by_user_id(
                users[user_index].id
            ) >= 0
        ) {

            send_text(
                client->fd,
                "This user is already logged in.\n"
            );

            disconnect_client(
                client_index
            );

            return 0;
        }
    }

    /*
     * New username.
     */
    else {

        if (user_count >= MAX_USERS) {

            send_text(
                client->fd,
                "User database is full.\n"
            );

            disconnect_client(
                client_index
            );

            return 0;
        }

        user_index = user_count++;

        users[user_index].id =
            get_max_user_id() + 1;

        strncpy(
            users[user_index].username,
            line,
            USERNAME_SIZE - 1
        );

        users[user_index].username[
            USERNAME_SIZE - 1
        ] = '\0';

        users[user_index].active = 1;
        users[user_index].is_admin = 0;

        if (save_users() < 0) {

            send_text(
                client->fd,
                "Could not save user.\n"
            );

            disconnect_client(
                client_index
            );

            return 0;
        }

        printf(
            "New user created: %s ID=%d\n",
            users[user_index].username,
            users[user_index].id
        );
    }

    client->user_id =
        users[user_index].id;

    client->logged_in = 1;

    client->mode =
        MODE_MENU;

    send_format(
        client->fd,
        "\nLogin successful.\n"
        "Welcome %s. User ID: %d%s\n",
        users[user_index].username,
        users[user_index].id,
        users[user_index].is_admin
            ? " [ADMIN]"
            : ""
    );

    show_conversation_list(
        client_index
    );

    return 1;
}


/*
 * ----------------------------------------------------------
 * MENU INPUT
 * ----------------------------------------------------------
 */

static int process_menu(
    int client_index,
    char *line
)
{
    struct Client *client =
        &clients[client_index];

    if (strcmp(line, "q") == 0) {

        send_text(
            client->fd,
            "Goodbye.\n"
        );

        disconnect_client(
            client_index
        );

        return 0;
    }

    if (strcmp(line, "r") == 0) {

        show_conversation_list(
            client_index
        );

        return 1;
    }

    if (strcmp(line, "a") == 0) {

        int my_index =
            find_user_index_by_id(
                client->user_id
            );

        if (
            my_index >= 0 &&
            users[my_index].is_admin
        ) {

            client->mode =
                MODE_ADMIN;

            show_admin_page(
                client_index
            );
        }
        else {

            send_text(
                client->fd,
                "Admin access denied.\n> "
            );
        }

        return 1;
    }

    /*
     * n mehmet
     */
    if (
        line[0] == 'n' &&
        line[1] == ' '
    ) {

        char *username =
            line + 2;

        int user_index =
            find_user_index_by_name(
                username
            );

        if (user_index == -1) {

            send_text(
                client->fd,
                "User does not exist.\n> "
            );

            return 1;
        }

        open_chat(
            client_index,
            users[user_index].id
        );

        return 1;
    }

    /*
     * Number selection.
     */
    char *end;

    long selection =
        strtol(
            line,
            &end,
            10
        );

    if (
        *line != '\0' &&
        *end == '\0'
    ) {

        if (
            selection >= 1 &&
            selection <=
                client->menu_contact_count
        ) {

            int partner_id =
                client->menu_contact_ids[
                    selection - 1
                ];

            open_chat(
                client_index,
                partner_id
            );

            return 1;
        }
    }

    send_text(
        client->fd,
        "Invalid command.\n> "
    );

    return 1;
}


/*
 * ----------------------------------------------------------
 * CHAT INPUT
 * ----------------------------------------------------------
 */

static int process_chat(
    int client_index,
    char *line
)
{
    struct Client *client =
        &clients[client_index];

    if (strcmp(line, "q") == 0) {

        client->mode =
            MODE_MENU;

        client->chat_partner_id = -1;

        show_conversation_list(
            client_index
        );

        return 1;
    }

    if (line[0] == '\0') {

        send_text(
            client->fd,
            "> "
        );

        return 1;
    }

    int partner_index =
        find_user_index_by_id(
            client->chat_partner_id
        );

    if (partner_index == -1) {

        send_text(
            client->fd,
            "This user no longer exists.\n> "
        );

        return 1;
    }

    /*
     * Removed account:
     *
     * history exists,
     * but new messages cannot be sent.
     */
    if (!users[partner_index].active) {

        send_format(
            client->fd,
            "%s is currently unavailable. "
            "The account has been removed.\n> ",
            users[partner_index].username
        );

        return 1;
    }

    if (
        append_message(
            client->user_id,
            users[partner_index].id,
            line
        ) < 0
    ) {

        send_text(
            client->fd,
            "Message could not be saved.\n> "
        );

        return 1;
    }

    /*
     * Recipient online?
     */
    int recipient_client =
        find_client_by_user_id(
            users[partner_index].id
        );

    if (recipient_client == -1) {

        send_format(
            client->fd,
            "[Stored. %s is offline.]\n> ",
            users[partner_index].username
        );

        return 1;
    }

    struct Client *recipient =
        &clients[recipient_client];

    int sender_index =
        find_user_index_by_id(
            client->user_id
        );

    const char *sender_name =
        sender_index >= 0
            ? users[sender_index].username
            : "unknown";

    /*
     * Recipient şu anda bizim chatimizi
     * açık tutuyorsa mesaj direkt ekrana.
     */
    if (
        recipient->mode == MODE_CHAT &&
        recipient->chat_partner_id ==
            client->user_id
    ) {

        send_format(
            recipient->fd,
            "\n%s: %s\n> ",
            sender_name,
            line
        );
    }

    /*
     * Başka sayfadaysa notification.
     */
    else {

        send_format(
            recipient->fd,
            "\n[New message from %s]\n> ",
            sender_name
        );
    }

    send_text(
        client->fd,
        "> "
    );

    return 1;
}


/*
 * ----------------------------------------------------------
 * ADMIN INPUT
 * ----------------------------------------------------------
 */

static int process_admin(
    int client_index,
    char *line
)
{
    struct Client *client =
        &clients[client_index];

    if (strcmp(line, "q") == 0) {

        client->mode =
            MODE_MENU;

        show_conversation_list(
            client_index
        );

        return 1;
    }

    char command;
    int selection;

    if (
        sscanf(
            line,
            "%c %d",
            &command,
            &selection
        ) != 2
    ) {

        send_text(
            client->fd,
            "Invalid admin command.\n"
            "admin> "
        );

        return 1;
    }

    if (
        selection < 1 ||
        selection > user_count
    ) {

        send_text(
            client->fd,
            "Invalid user number.\n"
            "admin> "
        );

        return 1;
    }

    int target_index =
        selection - 1;

    /*
     * REMOVE USER
     */
    if (command == 'd') {

        if (
            users[target_index].is_admin
        ) {

            send_text(
                client->fd,
                "Admin account cannot be removed.\n"
                "admin> "
            );

            return 1;
        }

        if (!users[target_index].active) {

            send_text(
                client->fd,
                "User is already removed.\n"
                "admin> "
            );

            return 1;
        }

        users[target_index].active = 0;

        save_users();

        /*
         * Eğer kullanıcı şu anda online ise
         * bağlantısını da kes.
         */
        int target_client =
            find_client_by_user_id(
                users[target_index].id
            );

        if (target_client >= 0) {

            send_text(
                clients[target_client].fd,
                "\nYour account has been removed "
                "by the administrator.\n"
            );

            disconnect_client(
                target_client
            );
        }

        send_format(
            client->fd,
            "%s removed.\n",
            users[target_index].username
        );

        show_admin_page(
            client_index
        );

        return 1;
    }

    /*
     * RESTORE USER
     */
    if (command == 'r') {

        if (users[target_index].active) {

            send_text(
                client->fd,
                "User is already active.\n"
                "admin> "
            );

            return 1;
        }

        users[target_index].active = 1;

        save_users();

        send_format(
            client->fd,
            "%s restored.\n",
            users[target_index].username
        );

        show_admin_page(
            client_index
        );

        return 1;
    }

    send_text(
        client->fd,
        "Unknown admin command.\n"
        "admin> "
    );

    return 1;
}


/*
 * ----------------------------------------------------------
 * PROCESS LINE
 * ----------------------------------------------------------
 */

static int process_line(
    int client_index,
    char *line
)
{
    sanitize_message(line);

    switch (
        clients[client_index].mode
    ) {

        case MODE_LOGIN:

            return process_login(
                client_index,
                line
            );

        case MODE_MENU:

            return process_menu(
                client_index,
                line
            );

        case MODE_CHAT:

            return process_chat(
                client_index,
                line
            );

        case MODE_ADMIN:

            return process_admin(
                client_index,
                line
            );
    }

    return 1;
}


/*
 * ----------------------------------------------------------
 * MAIN
 * ----------------------------------------------------------
 */

int main(void)
{
    int server_fd;

    struct sockaddr_in server_addr;

    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    memset(
        clients,
        0,
        sizeof(clients)
    );

    for (
        int i = 0;
        i < MAX_CLIENTS;
        i++
    ) {

        clients[i].fd = -1;
    }

    if (load_users() < 0) {

        fprintf(
            stderr,
            "Could not load users.\n"
        );

        return 1;
    }

    server_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server_fd < 0) {

        perror("socket");

        return 1;
    }

    int reuse = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(PORT);

    server_addr.sin_addr.s_addr =
        INADDR_ANY;

    if (
        bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0
    ) {

        perror("bind");

        close(server_fd);

        return 1;
    }

    if (
        listen(
            server_fd,
            16
        ) < 0
    ) {

        perror("listen");

        close(server_fd);

        return 1;
    }

    epoll_fd =
        epoll_create1(0);

    if (epoll_fd < 0) {

        perror("epoll_create1");

        close(server_fd);

        return 1;
    }

    memset(
        &event,
        0,
        sizeof(event)
    );

    event.events =
        EPOLLIN;

    event.data.fd =
        server_fd;

    if (
        epoll_ctl(
            epoll_fd,
            EPOLL_CTL_ADD,
            server_fd,
            &event
        ) < 0
    ) {

        perror("epoll_ctl");

        close(epoll_fd);
        close(server_fd);

        return 1;
    }

    printf(
        "Server listening on port %d...\n",
        PORT
    );

    printf(
        "Default admin: ahmet\n"
    );

    while (1) {

        int event_count =
            epoll_wait(
                epoll_fd,
                events,
                MAX_EVENTS,
                -1
            );

        if (event_count < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");

            break;
        }

        for (
            int i = 0;
            i < event_count;
            i++
        ) {

            int current_fd =
                events[i].data.fd;

            /*
             * ----------------------------------
             * NEW CONNECTION
             * ----------------------------------
             */
            if (
                current_fd ==
                server_fd
            ) {

                struct sockaddr_in client_addr;

                socklen_t client_len =
                    sizeof(client_addr);

                int client_fd =
                    accept(
                        server_fd,
                        (struct sockaddr *)
                            &client_addr,
                        &client_len
                    );

                if (client_fd < 0) {

                    perror("accept");
                    continue;
                }

                int client_index =
                    find_free_client_slot();

                if (client_index == -1) {

                    send_text(
                        client_fd,
                        "Server is full.\n"
                    );

                    close(client_fd);

                    continue;
                }

                memset(
                    &clients[client_index],
                    0,
                    sizeof(struct Client)
                );

                clients[client_index].fd =
                    client_fd;

                clients[client_index].active =
                    1;

                clients[client_index].logged_in =
                    0;

                clients[client_index].user_id =
                    -1;

                clients[client_index].mode =
                    MODE_LOGIN;

                clients[
                    client_index
                ].chat_partner_id = -1;

                char ip[
                    INET_ADDRSTRLEN
                ];

                inet_ntop(
                    AF_INET,
                    &client_addr.sin_addr,
                    ip,
                    sizeof(ip)
                );

                printf(
                    "Connection: %s:%d fd=%d\n",
                    ip,
                    ntohs(
                        client_addr.sin_port
                    ),
                    client_fd
                );

                memset(
                    &event,
                    0,
                    sizeof(event)
                );

                event.events =
                    EPOLLIN;

                event.data.fd =
                    client_fd;

                if (
                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_ADD,
                        client_fd,
                        &event
                    ) < 0
                ) {

                    perror(
                        "epoll_ctl client"
                    );

                    disconnect_client(
                        client_index
                    );

                    continue;
                }

                send_text(
                    client_fd,
                    "Enter your username: "
                );

                continue;
            }

            /*
             * ----------------------------------
             * FIND CLIENT
             * ----------------------------------
             */
            int client_index = -1;

            for (
                int j = 0;
                j < MAX_CLIENTS;
                j++
            ) {

                if (
                    clients[j].active &&
                    clients[j].fd ==
                        current_fd
                ) {

                    client_index = j;
                    break;
                }
            }

            if (client_index == -1) {
                continue;
            }

            /*
             * ----------------------------------
             * RECEIVE
             * ----------------------------------
             */
            char buffer[1024];

            ssize_t n =
                recv(
                    current_fd,
                    buffer,
                    sizeof(buffer),
                    0
                );

            if (n < 0) {

                if (errno == EINTR) {
                    continue;
                }

                perror("recv");

                disconnect_client(
                    client_index
                );

                continue;
            }

            if (n == 0) {

                if (
                    clients[
                        client_index
                    ].logged_in
                ) {

                    int user_index =
                        find_user_index_by_id(
                            clients[
                                client_index
                            ].user_id
                        );

                    if (user_index >= 0) {

                        printf(
                            "%s disconnected.\n",
                            users[
                                user_index
                            ].username
                        );
                    }
                }

                disconnect_client(
                    client_index
                );

                continue;
            }

            /*
             * TCP stream:
             *
             * recv() == message varsaymıyoruz.
             * Gelen byte'ları input buffer'a
             * ekliyoruz.
             */
            struct Client *client =
                &clients[client_index];

            if (
                client->input_len +
                (size_t)n >=
                sizeof(
                    client->input_buffer
                )
            ) {

                send_text(
                    client->fd,
                    "Input line too long.\n"
                );

                client->input_len = 0;

                continue;
            }

            memcpy(
                client->input_buffer +
                    client->input_len,
                buffer,
                (size_t)n
            );

            client->input_len +=
                (size_t)n;

            /*
             * Buffer'daki bütün tamamlanmış
             * satırları işle.
             */
            while (
                client->active
            ) {

                char *newline =
                    memchr(
                        client->input_buffer,
                        '\n',
                        client->input_len
                    );

                if (newline == NULL) {
                    break;
                }

                size_t line_length =
                    (size_t)(
                        newline -
                        client->input_buffer
                    );

                char line[
                    INPUT_BUFFER_SIZE
                ];

                memcpy(
                    line,
                    client->input_buffer,
                    line_length
                );

                line[line_length] =
                    '\0';

                size_t consumed =
                    line_length + 1;

                memmove(
                    client->input_buffer,
                    client->input_buffer +
                        consumed,
                    client->input_len -
                        consumed
                );

                client->input_len -=
                    consumed;

                if (
                    !process_line(
                        client_index,
                        line
                    )
                ) {

                    break;
                }
            }
        }
    }

    close(epoll_fd);
    close(server_fd);

    return 0;
}