/*
--------------------------------------------------
    James William Fletcher (github.com/mrbid)
--------------------------------------------------
        MAY 2024
        ~~~~~~~~

    - Disable game crosshair or change it to something that does not
      obscure the center of the screen and is not red.
    - Make sure anti-aliasing is OFF in VIDEO settings.
    - Turn off any mouse acceleration.
    - Use XORG not WAYLAND.

    The ZPixmap improvement which increases scan speeds considerably was contributed by:
    Test_User (https://notabug.org/test_user) (hax@andrewyu.org).
    
    Prereq: sudo apt install clang xterm libx11-dev libxdo-dev
    Compile: clang aim.c -Ofast -mfma -march=native -lX11 -lxdo -pthread -lm -o aim
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <X11/Xutil.h>
#include <xdo.h>

#include <pthread.h>
#include <fcntl.h>

#define uint unsigned int

// user configurable
#define FIND_WINDOW_STR "Quake"
#define COLOR_DETECT cr > 250 && cg < 13 && cb < 13
#define OPTION_DELAY_US 300000 // delay between option changes in micro-seconds (μs)
useconds_t SCAN_DELAY_US = 8000; // delay between screen captures in micro-seconds (μs)
uint64_t MOUSE_UPDATE_US = 16000; // delay between mouse movements in micro-seconds (μs)
uint mouse_smooth = 1; // mouse smoothing steps
float mouse_scaler = 8.f; // mouse movement scaler

// other
float sx[16],sy[16]; // smooth
uint smi=0; // smoothing index
xdo_t* xdo;
Display *d;
int si;
Window twin;
Window this_win = 0;
int cx=0, cy=0;
uint minimal = 0;
uint enable = 0;
uint crosshair = 1;
uint autoaim = 0;
uint64_t autoaim_start = 0;
uint sps = 0;
int sd=100;
int sd2=200;
int sd2m1=199;
uint lon = 0;

/***************************************************
   ~~ Utils
*/
int key_is_pressed(KeySym ks)
{
    // https://www.cl.cam.ac.uk/~mgk25/ucs/keysymdef.h
    // https://stackoverflow.com/questions/18281412/check-keypress-in-c-on-linux/52801588
    char keys_return[32];
    XQueryKeymap(d, keys_return);
    KeyCode kc2 = XKeysymToKeycode(d, ks);
    int isPressed = !!(keys_return[kc2 >> 3] & (1 << (kc2 & 7)));
    return isPressed;
}
uint64_t microtime()
{
    struct timeval tv;
    struct timezone tz;
    memset(&tz, 0, sizeof(struct timezone));
    gettimeofday(&tv, &tz);
    return 1000000 * tv.tv_sec + tv.tv_usec;
}
void loadConfig(const uint minimal)
{
    FILE* f = fopen("config.txt", "r");
    if(f)
    {
        char line[256];
        while(fgets(line, 256, f) != NULL)
        {
            char set[64];
            memset(set, 0, 64);
            float val;
            if(sscanf(line, "%63s %f", set, &val) == 2)
            {
                if(strcmp(set, "SCAN_DELAY_US") == 0){SCAN_DELAY_US = (useconds_t)val; if(minimal == 0){printf("Setting Loaded: \e[38;5;76m%s\e[38;5;123m \e[38;5;13m%g\e[38;5;123m\n", set, val);}}
                if(strcmp(set, "MOUSE_UPDATE_US") == 0){MOUSE_UPDATE_US = (uint64_t)val; if(minimal == 0){printf("Setting Loaded: \e[38;5;76m%s\e[38;5;123m \e[38;5;13m%g\e[38;5;123m\n", set, val);}}
                if(strcmp(set, "MOUSE_SCALE") == 0){mouse_scaler = val; if(minimal == 0){printf("Setting Loaded: \e[38;5;76m%s\e[38;5;123m \e[38;5;13m%g\e[38;5;123m\n", set, val);}}
                if(strcmp(set, "MOUSE_SMOOTH") == 0){mouse_smooth = (uint)val; if(minimal == 0){printf("Setting Loaded: \e[38;5;76m%s\e[38;5;123m \e[38;5;13m%g\e[38;5;123m\n", set, val);}}
            }
        }
        fclose(f);
        if(minimal == 0){printf("\n");}
    }
}

/***************************************************
   ~~ X11 Utils
*/
int left=0, right=0, middle=0, four=0;
void *mouseThread(void *arg)
{
    // https://stackoverflow.com/a/23317086
    int fd = open("/dev/input/mice", O_RDWR);
    if(fd == -1)
    {
        printf("Failed to open '/dev/input/mice' mouse input is non-operable.\nTry to execute as superuser (sudo) or: chmod 0777 /dev/input/mice.\n");
        return 0;
    }
    unsigned char data[3];
    while(1)
    {
        int bytes = read(fd, data, sizeof(data));
        if(bytes > 0)
        {
            left = data[0] & 0x1;
            right = data[0] & 0x2;
            middle = data[0] & 0x4;
            four = data[0] & 0x5;
        }
        //printf("%i, %i, %i, %i\n", left, right, middle, four);
    }
    close(fd);
}
Window getWindow(Display* d, const int si) // gets child window mouse is over
{
    XEvent event;
    memset(&event, 0x00, sizeof(event));
    XQueryPointer(d, RootWindow(d, si), &event.xbutton.root, &event.xbutton.window, &event.xbutton.x_root, &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y, &event.xbutton.state);
    event.xbutton.subwindow = event.xbutton.window;
    while(event.xbutton.subwindow)
    {
        event.xbutton.window = event.xbutton.subwindow;
        XQueryPointer(d, event.xbutton.window, &event.xbutton.root, &event.xbutton.subwindow, &event.xbutton.x_root, &event.xbutton.y_root, &event.xbutton.x, &event.xbutton.y, &event.xbutton.state);
    }
    return event.xbutton.window;
}
Window findWindow(Display *d, Window current, char const *needle)
{
    // https://www.unix.com/programming/254680-xlib-search-window-his-name.html
    Window ret = 0, root, parent, *children;
    char *name = NULL;
    if(current == 0){current = XDefaultRootWindow(d);}
    if(XFetchName(d, current, &name) > 0)
    {
        if(strstr(name, needle) != NULL){XFree(name);return current;}
        XFree(name);
    }
    uint cc;
    if(XQueryTree(d, current, &root, &parent, &children, &cc) != 0)
    {
        for(uint i=0; i < cc; ++i)
        {
            Window win = findWindow(d, children[i], needle);
            if(win != 0){ret=win;break;}
        }
        XFree(children);
    }
    return ret;
}
#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD    1
#define _NET_WM_STATE_TOGGLE 2 
Bool MakeAlwaysOnTop(Display* display, Window root, Window mywin)
{
    // https://stackoverflow.com/a/16235920
    Atom wmStateAbove = XInternAtom(display, "_NET_WM_STATE_ABOVE", 1);
    if(wmStateAbove == None){return False;}
    Atom wmNetWmState = XInternAtom(display, "_NET_WM_STATE", 1);
    if(wmNetWmState == None){return False;}
    if(wmStateAbove != None)
    {
        XClientMessageEvent xclient;
        memset(&xclient, 0, sizeof(xclient));
        xclient.type = ClientMessage;
        xclient.window = mywin;
        xclient.message_type = wmNetWmState;
        xclient.format = 32;
        xclient.data.l[0] = _NET_WM_STATE_ADD;
        xclient.data.l[1] = wmStateAbove;
        xclient.data.l[2] = 0;
        xclient.data.l[3] = 0;
        xclient.data.l[4] = 0;
        XSendEvent(display, root, False, SubstructureRedirectMask|SubstructureNotifyMask, (XEvent*)&xclient);
        XFlush(display);
        return True;
    }
    return False;
}
Window getNextChild(Display* d, Window current)
{
    uint cc = 0;
    Window root, parent, *children;
    if(XQueryTree(d, current, &root, &parent, &children, &cc) == 0){return current;}
    const Window rw = children[0];
    XFree(children);
    //printf("%lX\n", children[i]);
    return rw;
}

/***************************************************
   ~~ Where The Magic Happens
*/
void targetEnemy()
{
    // get image block
    XImage *img = XGetImage(d, twin, cx-sd, cy-sd, sd2, sd2, AllPlanes, ZPixmap);
    if(img == NULL){return;}
    
    // increment scans per second
    sps++;

    // top left detection
    int ax = 0, ay = 0;
    uint b1 = 0;
    for(int y = 0; y < sd2; y++)
    {
        for(int x = 0; x < sd2; x++)
        {
            const unsigned char cr = img->data[(y*4*sd2)+(x*4) + 2];
            const unsigned char cg = img->data[(y*4*sd2)+(x*4) + 1];
            const unsigned char cb = img->data[(y*4*sd2)+(x*4) + 0];
            if(COLOR_DETECT)
            {
                ax = x, ay = y;
                b1 = 1;
                break;
            }
        }
        if(b1 == 1){break;}
    }

    // early exit
    if(b1 == 0)
    {
        XDestroyImage(img);
        sd = 100;
        sd2 = 200;
        sd2m1 = 199;
        lon = 0;
        return;
    }

    // bottom right detection
    int bx = 0, by = 0;
    uint b2 = 0;
    for(int y = sd2m1; y > 0; y--)
    {
        for(int x = sd2m1; x > 0; x--)
        {
            const unsigned char cr = img->data[(y*4*sd2)+(x*4) + 2];
            const unsigned char cg = img->data[(y*4*sd2)+(x*4) + 1];
            const unsigned char cb = img->data[(y*4*sd2)+(x*4) + 0];
            if(COLOR_DETECT)
            {
                bx = x, by = y;
                b2 = 1;
                break;
            }
        }
        if(b2 == 1){break;}
    }

    // early exit
    if(b2 == 0)
    {
        XDestroyImage(img);
        return;
    }

    // target it
    if(b1 == 1 && b2 == 1)
    {
        lon = 1;

        // center on target
        const int dx = abs(ax-bx)/2;
        const int ady = abs(ay-by);
        const int dy = ady/2;
        int mx = (ax-sd)+dx;
        int my = (ay-sd)+dy;
        if(mouse_smooth != 0)
        {
            static float tsx, tsy;
            tsx=0.f,tsy=0.f;
            for(uint i=0; i < mouse_smooth; i++)
            {
                tsx += sx[i];
                tsy += sy[i];
            }
            tsx += roundf(((float)mx)*mouse_scaler);
            tsy += roundf(((float)my)*mouse_scaler);
            tsx /= mouse_smooth+1;
            tsy /= mouse_smooth+1;
            sx[smi] = tsx;
            sy[smi] = tsy;
            if(++smi > mouse_smooth-1){smi=0;}
            mx = (int)tsx;
            my = (int)tsy;
        }
        else
        {
            mx += roundf(((float)mx)*mouse_scaler);
            my += roundf(((float)my)*mouse_scaler);
        }

        // resize scan window
        if(ady > 6 && ady < 100)
        {
            sd  = (sd+ady)/2; // smoothed
            sd2 = sd*2;
            sd2m1 = sd2-1;
        }
        else if(ady <= 6)
        {
            sd  = 6;
            sd2 = 12;
            sd2m1 = 11;
        }

        // only move the mouse if one of the mx or my is > 0
        static uint64_t lt = 0;
        if((mx != 0 || my != 0) && microtime()-lt > MOUSE_UPDATE_US)
        {
            xdo_move_mouse_relative(xdo, mx, my);
            lt = microtime();
        }

        // auto aim smoothing reset
        if(autoaim == 1){autoaim_start = microtime();}
    }

    // done
    XDestroyImage(img);
}

/***************************************************
   ~~ Program Entry Point
*/
void rainbow_line_printf(const char* text)
{
    static unsigned char base_clr = 0;
    if(base_clr == 0){base_clr = (rand()%125)+55;}
    printf("\e[38;5;%im", base_clr);
    if(++base_clr >= 230){base_clr = (rand()%125)+55;}
    const uint len = strlen(text);
    for(uint i = 0; i < len; i++){printf("%c", text[i]);}
    printf("\e[38;5;123m");
}
void reprint()
{
    system("clear");
    if(minimal == 0)
    {
        printf("\033[1m\033[0;31m>>> Quake Aimbot <<<\e[0m\n");
        rainbow_line_printf("James William Fletcher (github.com/mrbid)\n\n");
        rainbow_line_printf("L-CTRL + L-ALT = Toggle BOT ON/OFF\n");
        rainbow_line_printf("MOUSE1/MOUSE3/MOUSE4 = Target enemy.\n");
        rainbow_line_printf("[ = Toggle Auto-Aim.\n");
        rainbow_line_printf("] = Toggle crosshair.\n");
        rainbow_line_printf("\\ = Hold pressed to print scans per second.\n");
        rainbow_line_printf("\nDisable the game crosshair.\n");
        rainbow_line_printf("> If your monitor provides a crosshair that will work fine.\n");
        rainbow_line_printf("> OR just use the crosshair this bot provides.\n");
        printf("\e[38;5;76m\n");
        printf("- Make sure anti-aliasing is OFF in VIDEO settings.\n");
        printf("- Turn off mouse acceleration.\n");
        printf("- This targets \033[1m\033[0;31mred\e[38;5;76m things.\n");
        printf("\n\e[38;5;123m");
        if(twin != 0)
        {
            printf("Target Win: 0x%lX\n\n", twin);

            if(enable == 1)
                rainbow_line_printf("BOT: \033[1m\e[32mON\e[0m\n");
            else
                rainbow_line_printf("BOT: \033[1m\e[31mOFF\e[0m\n");

            if(autoaim == 1)
                rainbow_line_printf("AUTOAIM: \033[1m\e[32mON\e[0m\n");
            else
                rainbow_line_printf("AUTOAIM: \033[1m\e[31mOFF\e[0m\n");

            if(crosshair == 1)
                rainbow_line_printf("CROSSHAIR: \033[1m\e[32mON\e[0m\n");
            else
                rainbow_line_printf("CROSSHAIR: \033[1m\e[31mOFF\e[0m\n");

            printf("\n");
        }
    }
    else
    {
        if(twin != 0)
        {
            if(enable == 1)
                printf(" \e[38;5;%umBOT: \033[1m\e[32mON\e[0m | ", minimal);
            else
                printf(" \e[38;5;%umBOT: \033[1m\e[31mOFF\e[0m | ", minimal);

            if(autoaim == 1)
                printf("\e[38;5;%umAUTOAIM: \033[1m\e[32mON\e[0m | ", minimal);
            else
                printf("\e[38;5;%umAUTOAIM: \033[1m\e[31mOFF\e[0m | ", minimal);

            if(crosshair == 1)
                printf("\e[38;5;%umCROSSHAIR: \033[1m\e[32mON\e[0m ", minimal);
            else
                printf("\e[38;5;%umCROSSHAIR: \033[1m\e[31mOFF\e[0m ", minimal);

            fflush(stdout);
        }
        else
        {
            printf("\e[38;5;123mPress \e[38;5;76mL-CTRL\e[38;5;123m + \e[38;5;76mL-ALT\e[38;5;123m to enable bot.\e[0m");
            fflush(stdout);
        }
    }
}
int main(int argc, char *argv[])
{
    // some init stuff
    srand(time(0));

    // is minimal ui?
    if(argc == 2)
    {
        minimal = atoi(argv[1]);
        if(minimal == 1){minimal = 8;}
        else if(minimal == 8){minimal = 1;}
    }

    // intro
    reprint();

    //

    GC gc = 0;
    time_t ct = time(0);

    //

    // try to open the default display
    d = XOpenDisplay(getenv("DISPLAY")); // explicit attempt on environment variable
    if(d == NULL)
    {
        d = XOpenDisplay((char*)NULL); // implicit attempt on environment variable
        if(d == NULL)
        {
            d = XOpenDisplay(":0"); // hedge a guess
            if(d == NULL)
            {
                printf("Failed to open display :'(\n");
                return 0;
            }
        }
    }

    // get default screen
    si = XDefaultScreen(d);

    //xdo
    xdo = xdo_new_with_opened_display(d, (char*)NULL, 0);
    time_t launch_time = 0;
    if(minimal > 0){launch_time = time(0);}

    // get graphics context
    gc = DefaultGC(d, si);

    // find bottom window
    twin = findWindow(d, 0, FIND_WINDOW_STR);
    if(twin != 0){reprint();}

    // load config
    loadConfig(minimal);
    if(mouse_smooth > 16){mouse_smooth = 16;}
    for(uint i=0; i < mouse_smooth; i++)
        sx[i] = mouse_scaler, sy[i] = mouse_scaler;

    // start mouse thread
    pthread_t tid;
    if(pthread_create(&tid, NULL, mouseThread, NULL) != 0)
    {
        printf("pthread_create(mouseThread) failed.\n");
        return 0;
    }
    
    // begin bot
    while(1)
    {
        // loop every 1 ms (1,000 microsecond = 1 millisecond)
        usleep(SCAN_DELAY_US);

        // do minimal?
        if(launch_time != 0 && time(0)-launch_time >= 1)
        {
            xdo_get_active_window(xdo, &this_win);
            xdo_set_window_property(xdo, this_win, "WM_NAME", "> Quake Aimbot <");
            xdo_set_window_size(xdo, this_win, 260, 1, 0);
            MakeAlwaysOnTop(d, XDefaultRootWindow(d), this_win);
            launch_time = 0;
        }

        // lockon timeout
        const int isp = four > 0;
        if(autoaim == 1 && microtime()-autoaim_start > 333000)
        {
            sd = 100, sd2 = 200, sd2m1 = 199;
            for(uint i=0; i < mouse_smooth; i++)
                sx[i] = mouse_scaler, sy[i] = mouse_scaler;
        }
        else if(left == 0 && isp == 0 && autoaim == 0)
        {
            sd = 100, sd2 = 200, sd2m1 = 199, lon = 0; // reset scan
            for(uint i=0; i < mouse_smooth; i++)
                sx[i] = mouse_scaler, sy[i] = mouse_scaler;
        }

        // inputs
        if(key_is_pressed(XK_Control_L) && key_is_pressed(XK_Alt_L))
        {
            if(enable == 0)
            {                
                // get window
                twin = findWindow(d, 0, FIND_WINDOW_STR);
                if(twin != 0)
                    twin = getNextChild(d, twin);
                else
                    twin = getWindow(d, si);

                if(twin == 0)
                {
                    if(minimal == 0)
                    {
                        printf("Failed to detect window.\n");
                    }
                    else
                    {
                        system("clear");
                        printf("Failed to detect window.");
                        fflush(stdout);
                    }
                }

                // get center window point (x & y)
                XWindowAttributes attr;
                XGetWindowAttributes(d, twin, &attr);
                cx = attr.width/2;
                cy = attr.height/2;

                // toggle
                enable = 1;
                usleep(OPTION_DELAY_US);
                reprint();
            }
            else
            {
                enable = 0;
                usleep(OPTION_DELAY_US);
                reprint();
            }
        }
        
        // bot on/off
        if(enable == 1)
        {
            // always tracks sps
            const int spson = key_is_pressed(XK_backslash);
            static uint64_t st = 0;
            if(microtime() - st >= 1000000)
            {
                if(spson == 1)
                {
                    if(minimal > 0){system("clear");}
                    printf("\e[36mSPS: %u\e[0m", sps);
                    if(minimal == 0){printf("\n");}else{fflush(stdout);}
                }
                sps = 0;
                st = microtime();
            }

            // autoaim toggle
            if(key_is_pressed(XK_bracketleft))
            {
                if(autoaim == 0)
                {
                    autoaim = 1;
                    usleep(OPTION_DELAY_US);
                    reprint();
                }
                else
                {
                    autoaim = 0;
                    usleep(OPTION_DELAY_US);
                    reprint();
                }
            }

            // crosshair toggle
            if(key_is_pressed(XK_bracketright))
            {
                if(crosshair == 0)
                {
                    crosshair = 1;
                    usleep(OPTION_DELAY_US);
                    reprint();
                }
                else
                {
                    crosshair = 0;
                    usleep(OPTION_DELAY_US);
                    reprint();
                }
            }

            // target
            if(isp == 0)
            {
                for(uint i=0; i < mouse_smooth; i++)
                    sx[i] = mouse_scaler, sy[i] = mouse_scaler;
            }
            if(spson == 1 || left == 1 || autoaim == 1 || isp == 1)
                targetEnemy();

            // crosshair
            if(crosshair == 1)
            {
                if(lon == 0)
                    XSetForeground(d, gc, 0x00FF00);
                else
                    XSetForeground(d, gc, 0xFF0000);
                XDrawRectangle(d, twin, gc, cx-sd-1, cy-sd-1, sd2+2, sd2+2);
                XFlush(d);
            }
        }

    }

    // done, never gets here in regular execution flow
    XCloseDisplay(d);
    return 0;
}

