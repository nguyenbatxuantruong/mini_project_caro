#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <termios.h>
#include <signal.h>

#define UP 65
#define DOWN 66
#define LEFT 68
#define RIGHT 67
#define CHAT '`'
#define MAX_TIME 20

#define HOST_IP "127.0.0.1"
#define PORT 6996
#define MAX 128
#define MAP_SIZE 8

#define lock pthread_mutex_lock
#define unlock pthread_mutex_unlock
#define xy(x, y) printf("\033[%d;%dH", x, y)
#define clear_eol(x) print(x, 12, "\033[K")

typedef struct
{
    int sockfd;
} socket_thread_args;

typedef struct
{
    int x;
    int y;
} pos; //vi tri trong ban co

char my_chess_man = 'x';
char op_chess_man = 'o';
char game_message[MAX];

int sv_fd;
int cli_fd;
int game_mode;
int game_timer = MAX_TIME;
int counter;            /*Only can be changed when end_turn == 0*/
int val[MAP_SIZE][MAP_SIZE];

pos cursor;
pos this_turn;          /*Only can be changed when end_turn == 0*/

bool game_ticked = 0;   /*Only can be changed when end_turn == 0*/
bool end_turn = 0;      /* sync socket - control - display*/
bool game_event = 0;    /* sync socket - display */
bool mess_event = 0;    /* sync socket - control */

pthread_mutex_t mutex_val = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_end_turn = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_game_event = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_mess_event = PTHREAD_MUTEX_INITIALIZER;


static void game_exit()
{
    printf("Close socket...\n");
    if (1 == game_mode)
    {
        close(sv_fd);
    }
    else
    {
        close(cli_fd);
    }
    system("reset");
    printf("Quit game\n");
    exit(EXIT_SUCCESS);
}

static void game_signal_handler(int signo)
{
    game_exit();
}

char getch(void)
{
    char buf = 0;
    struct termios old = {0};
    fflush(stdout);
    if (tcgetattr(0, &old) < 0)
        perror("tcsetattr()");
    old.c_lflag &= ~ICANON;
    old.c_lflag &= ~ECHO;
    old.c_cc[VMIN] = 1;
    old.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &old) < 0)
        perror("tcsetattr ICANON");
    if (read(0, &buf, 1) < 0)
        perror("read()");
    old.c_lflag |= ICANON;
    old.c_lflag |= ECHO;
    if (tcsetattr(0, TCSADRAIN, &old) < 0)
        perror("tcsetattr ~ICANON");
    //printf("%c\n", buf);
    return buf;
}

void print(int y, int x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    xy(y + 1, x + 1), vprintf(fmt, ap);
    fflush(stdout);
}

static void game_processor_init_map()
{
    system("clear");
    int i, j;
    for (i = 0; i < 4 * MAP_SIZE; i++)
        for (j = 0; j <= 2 * MAP_SIZE; j += 2)
            print(j, i, "--");
    for (i = 0; i <= 4 * MAP_SIZE; i += 4)
        for (j = 0; j <= 2 * MAP_SIZE; j++)
            print(j, i, "|");

    cursor.x = 18;
    cursor.y = 9;
    print(cursor.y, cursor.x, "");

    lock(&mutex_val);
    for (i = 0; i < MAP_SIZE; i++)
        for (j = 0; j < MAP_SIZE; j++)
            val[i][j] = 0;
    unlock(&mutex_val);

    for (i = 0; i < MAP_SIZE; i++)
        for (j = 0; j < MAP_SIZE; j++)
        {
            print(j * 2 + 1, i * 4 + 2, "");
        }
    print(cursor.y, cursor.x, "");
}

static int game_processor_vertical_check(int x, int y)
{
    int count = 1;
    int a = x + 1;
    while (a < MAP_SIZE && val[x][y] == val[a][y])
    {
        a++;
        count++;
    }
    a = x - 1;
    while (a >= 0 && val[x][y] == val[a][y])
    {
        a--;
        count++;
    }
    return (count == 5);
}

static int game_processor_horizontal_check(int x, int y)
{
    int count = 1;
    int a = y + 1;
    while (a < MAP_SIZE && val[x][y] == val[x][a])
    {
        a++;
        count++;
    }
    a = y - 1;
    while (a >= 0 && val[x][y] == val[x][a])
    {
        a--;
        count++;
    }
    return (count == 5);
}

static int game_processor_diagonal_check(int x, int y)
{
    int count1 = 1, count2 = 1;
    int a = x + 1;
    int b = y + 1;
    while (a < MAP_SIZE && b < MAP_SIZE && val[x][y] == val[a][b])
    {
        a++;
        b++;
        count1++;
    }
    a = x - 1;
    b = y - 1;
    while (a >= 0 && b >= 0 && val[x][y] == val[a][b])
    {
        a--;
        b--;
        count1++;
    }

    a = x + 1;
    b = y - 1;
    while (a < MAP_SIZE && b >= 0 && val[x][y] == val[a][b])
    {
        a++;
        b--;
        count2++;
    }
    a = x - 1;
    b = y + 1;
    while (a >= 0 && b < MAP_SIZE && val[x][y] == val[a][b])
    {
        a--;
        b++;
        count2++;
    }
    return (count1 == 5 || count2 == 5);
}

static void game_processor_check_win(int x, int y)
{
    int win = 0;
    if ((val[x][y] != 0) && (game_processor_vertical_check(x, y) || game_processor_horizontal_check(x, y) || game_processor_diagonal_check(x, y)))
        win = val[x][y];
    else
        return;
    if (win == 1)
    {
        sleep(1);
        system("clear");
        print(0, 0, "You win");
        sleep(1);
        system("clear");
        game_processor_init_map();
        counter = 0;
        end_turn = 1;
    }
    else if (win == -1)
    {
        sleep(1);
        system("clear");
        print(0, 0, "Opponent win");
        sleep(1);
        system("clear");
        game_processor_init_map();
        counter = 0;
        end_turn = 0;
    }
}

static void game_processor_tick(int x, int y)
{
    if (x < 0 || x > (MAP_SIZE - 1))
    {
        return;
    }
    if (y < 0 || y > (MAP_SIZE - 1))
    {
        return;
    }
    if (val[x][y] == 0)
    {
        lock(&mutex_val);
        val[x][y] = 1;
        unlock(&mutex_val);
        counter++;
        this_turn.x = x;
        this_turn.y = y;
        game_ticked = 1;
        game_timer = MAX_TIME;
    }
}

static void *game_control()
{
    char input;
    int i, j;
    while(1)
    {
        input = getch();
        if (UP == input)
        {
            if (cursor.y > 2)
            {
                cursor.x = cursor.x;
                cursor.y = cursor.y - 2;
            }
            print(cursor.y, cursor.x, "");
        }
        else if (DOWN == input)
        {
            if (cursor.y < 2 * MAP_SIZE - 1)
            {
                cursor.x = cursor.x;
                cursor.y = cursor.y + 2;
            }
            print(cursor.y, cursor.x, "");
        }
        else if (LEFT == input)
        {
            if (cursor.x > 2)
            {
                cursor.x = cursor.x - 4;
                cursor.y = cursor.y;
            }
            print(cursor.y, cursor.x, "");
        }
        else if (RIGHT == input)
        {
            if (cursor.x < 4 * MAP_SIZE - 2)
            {
                cursor.x = cursor.x + 4;
                cursor.y = cursor.y;
            }
            print(cursor.y, cursor.x, "");
        }
        else if (CHAT == input)
        {
            lock(&mutex_mess_event);
            mess_event = 1;
            i = 0;
            print(2 * MAP_SIZE + 5, 0, "");
            scanf(" %[^\n]s",game_message);
            /* scanf(" %s", game_message);*/
            print(2 * MAP_SIZE + 5, 0, "                                                                                                                               ");
            print(cursor.y, cursor.x, "");
            unlock(&mutex_mess_event);
        }
        else if (my_chess_man == input)
        {
            i = (cursor.x - 2) / 4;
            j = (cursor.y - 1) / 2;
            lock(&mutex_end_turn);
            if (end_turn == 0)
            {
                game_processor_tick(i, j);
            }
            unlock(&mutex_end_turn);
        }
    }
    pthread_exit(NULL);
}

static void *game_timer_counter()
{
    // char buff[16];
    while(1)
    {
        if(game_timer <= 0)
        {
            system("clear");
            printf("Where are u now??\n");
            sleep(1);
            game_exit();
        }
        else
        {
            if(0 == end_turn)
            {
                game_timer--;
                // bzero(buff, sizeof(buff));
                // sprintf(buff, "TIME: \t%2d", game_timer);
                // print(2 * MAP_SIZE + 2, 16, buff);
                // print(cursor.y, cursor.x, "");
            }
            sleep(1);
        }
    }
    pthread_exit(NULL);
}

static void *game_display()
{
    int i, j;
    char s1[2], s2[2];
    s1[0] = my_chess_man;
    s1[1] = '\0';
    s2[0] = op_chess_man;
    s2[1] = '\0';
    while (1)
    {
        lock(&mutex_game_event);
        if (1 == game_event)
        {
            for (i = 0; i < MAP_SIZE; i++)
            {
                for (j = 0; j < MAP_SIZE; j++)
                {
                    if (val[i][j] == 1)
                        print(j * 2 + 1, i * 4 + 2, s1);
                    if (val[i][j] == 0)
                        print(j * 2 + 1, i * 4 + 2, "");
                    if (val[i][j] == -1)
                        print(j * 2 + 1, i * 4 + 2, s2);
                }
            }
            game_processor_check_win(this_turn.x, this_turn.y);
            lock(&mutex_end_turn);
            if (1 == end_turn)
            {
                print(2 * MAP_SIZE + 2, 0, "Op   turn: ");
            }
            else
            {
                print(2 * MAP_SIZE + 2, 0, "Your turn: ");
            }
            unlock(&mutex_end_turn);
            print(cursor.y, cursor.x, "");
            game_event = 0;
        }
        unlock(&mutex_game_event);
        usleep(100000);
    }
    pthread_exit(NULL);
}

static void *socket_read(void *_args)
{
    socket_thread_args *thr_args = (socket_thread_args *)_args;
    char buff[MAX];
    int sockfd = thr_args->sockfd;
    int rc;
    while (1)
    {
        usleep(500000);
        bzero(buff, sizeof(buff));
        rc = read(sockfd, buff, sizeof(buff));
        if (rc <= 0)
        {
            system("clear");
            printf("Disconnect\n");
            sleep(1);
            game_exit();
        }
        else
        {
            lock(&mutex_end_turn);
            if (1 == end_turn && buff[0] == 'O' && buff[1] == 'p' && buff[2] == ':')
            {
                end_turn = 0;
                sscanf(buff, "Op: %d, %d, %d", &counter, &this_turn.x, &this_turn.y);
                
                lock(&mutex_val);
                val[this_turn.x][this_turn.y] = -1;
                unlock(&mutex_val);

                print(2 * MAP_SIZE + 3, 0, "Move: ");
                print(2 * MAP_SIZE + 3, 12, buff);
                print(cursor.y, cursor.x, "");

                lock(&mutex_game_event);
                game_event = 1;
                unlock(&mutex_game_event);
            }
            else
            {
                print(2 * MAP_SIZE + 4, 0, "Competitor:                                                                   ");
                print(2 * MAP_SIZE + 4, 12, buff);
                print(cursor.y, cursor.x, "");
            }
            unlock(&mutex_end_turn);
        }
    }
    pthread_exit(NULL);
}

static void *socket_write(void *_args)
{
    socket_thread_args *thr_args = (socket_thread_args *)_args;
    char buff[MAX];
    int sockfd = thr_args->sockfd;
    while (1)
    {
        lock(&mutex_end_turn);
        if (0 == end_turn && game_ticked == 1)
        {
            end_turn = 1;
            game_ticked = 0;
            bzero(buff, sizeof(buff));
            sprintf(buff, "Op: %d, %d, %d\n", counter, this_turn.x, this_turn.y);
            send(sockfd, buff, strlen(buff), 0);

            lock(&mutex_game_event);
            game_event = 1;
            unlock(&mutex_game_event);
        }
        unlock(&mutex_end_turn);
        lock(&mutex_mess_event);
        if(1 == mess_event)
        {
            mess_event = 0;
            strcpy(buff, game_message);
            send(sockfd, buff, strlen(buff), 0);
        }
        unlock(&mutex_mess_event);
    }
    pthread_exit(NULL);
}

static void game_main_cheese_man_config()
{
    system("clear");
    printf("Choose your cheese man, bro\n");
    scanf(" %c", &my_chess_man);
    fflush(stdin);
    printf("Choose your opponent's cheese man, bro\n");
    scanf(" %c", &op_chess_man);
    return;
}

static int game_main_intro()
{
    int game_mod;
    system("clear");
    printf("1. Create game\n");
    printf("2. Join game\n");
    scanf(" %d", &game_mod);
    return game_mod;
}

static int game_mode_create()
{
    pthread_t threadID[4];
    socket_thread_args thr[2];

    int conn_fd;
    int len;
    int i;
    struct sockaddr_in sever_addr, client_addr;

    /* socket */
    sv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sv_fd < 0)
    {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }

    /* struct */
    bzero(&sever_addr, sizeof(sever_addr));
    sever_addr.sin_family = AF_INET;
    sever_addr.sin_addr.s_addr = INADDR_ANY;
    sever_addr.sin_port = htons(PORT);

    /* blind */
    if (bind(sv_fd, (struct sockaddr *)&sever_addr, sizeof(sever_addr)) < 0)
    {
        perror("Bind error");
        exit(EXIT_FAILURE);
    }

    /* listen */
    if (listen(sv_fd, 3) < 0)
    {
        perror("Listen error");
        exit(EXIT_FAILURE);
    }

    /* accept */
    len = sizeof(client_addr);
    conn_fd = accept(sv_fd, (struct sockaddr *)&client_addr, (socklen_t *)&len);
    if (conn_fd < 0)
    {
        perror("Accept error");
        exit(EXIT_FAILURE);
    }

    /* init game */
    game_processor_init_map();

    /* read & send */
    thr[0].sockfd = conn_fd;
    thr[1].sockfd = conn_fd;
    pthread_create(&threadID[0], NULL, socket_read, &thr[0]);
    pthread_create(&threadID[1], NULL, socket_write, &thr[1]);
    pthread_create(&threadID[2], NULL, game_control, NULL);
    pthread_create(&threadID[3], NULL, game_display, NULL);
    pthread_create(&threadID[4], NULL, game_timer_counter, NULL);
    for (i = 0; i < 4; i++)
    {
        pthread_join(threadID[i], NULL);
    }
    close(sv_fd);
    return 0;
}

static int game_mode_join()
{
    pthread_t threadID[4];
    socket_thread_args thr[2];

    struct sockaddr_in sever_addr;
    char ip_addr[16];
    int i;

    printf("Ip address: \n");
    scanf(" %s", ip_addr);

    /* socket */
    cli_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cli_fd < 0)
    {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }

    /* struct */
    bzero(&sever_addr, sizeof(sever_addr));
    sever_addr.sin_family = AF_INET;
    sever_addr.sin_addr.s_addr = inet_addr(ip_addr);
    sever_addr.sin_port = htons(PORT);

    /* connect */
    if (connect(cli_fd, (struct sockaddr *)&sever_addr, sizeof(sever_addr)) < 0)
    {
        perror("Connect error");
        exit(EXIT_FAILURE);
    }

    /* init game */
    game_processor_init_map();
    end_turn = 1;

    /* read & send */
    thr[0].sockfd = cli_fd;
    thr[1].sockfd = cli_fd;
    pthread_create(&threadID[0], NULL, socket_read, &thr[0]);
    pthread_create(&threadID[1], NULL, socket_write, &thr[1]);
    pthread_create(&threadID[2], NULL, game_control, NULL);
    pthread_create(&threadID[3], NULL, game_display, NULL);
    pthread_create(&threadID[4], NULL, game_timer_counter, NULL);
    for (i = 0; i < 5; i++)
    {
        pthread_join(threadID[i], NULL);
    }
    close(cli_fd);
    return 0;
}

static void game_main_processor()
{
    if (1 == game_mode)
    {
        printf("Waiting for other player ...\n");
        game_mode_create();
    }
    else
    {
        printf("Connect server ...\n");
        game_mode_join();
    }
    return;
}

int main()
{
    // game_main_cheese_man_config();
    game_mode = game_main_intro();
    signal(SIGINT, game_signal_handler);
    game_main_processor();

    game_exit();
    return 0;
}