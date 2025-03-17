#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DEVICE_PATH "/dev/wii_remote"
#define MAX_READ_SIZE 256

// Function to simulate mouse movement using xdotool
void send_mouse_move(int x, int y) {
    char command[100];
    snprintf(command, sizeof(command), "xdotool mousemove %d %d", x, y);
    system(command); // Execute the command in the shell
}

void left_click() {
    system("xdotool click 1"); // Simulate Left Click (1 = Left Mouse Button)
    usleep(200000); // Sleep to prevent multiple clicks  ( microseconds instead of seconds )
}

void right_click() {
    system("xdotool click 3"); // Simulate Right Click (3 = Right Mouse Button)
    usleep(200000); 
}

void page_up() {
    system("xdotool key Page_Up"); 
    usleep(200000); 
}

void page_down(){
    sytem("xdotool key Page_Down");
    usleep(200000);
}
int main() {
    int fd = open(DEVICE_PATH, O_RDONLY); // O_RDONLY flag that tells system to opwn in readonly mode
    if (fd == -1) {
        perror("Failed to open device");
        return 1;
    }


    printf("Key:\nDpad: move \nA: Left Click \nB: Right Click \n1: Page Up \n2: Page Down \n+: Dpi Up \n-: Dpi Down\n\nReading Wii Remote input...\n");

    char buffer[MAX_READ_SIZE];
    int x_pos = 0, y_pos = 0;
    int move_step = 20;  // Number of pixels to move per button press

    while (1) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer)-1);
        if (bytes_read < 0) {
            perror("Error reading from device");
            close(fd);
            break;
        }

        buffer[bytes_read] = '\0';


        // Look for D-pad button presses in the circular buffer      
        if (strstr(buffer, "Dpad_Down")) {
            printf("D-Pad Down pressed\n");
            y_pos += move_step;
        }
        else if (strstr(buffer, "Dpad_Up")) {
            printf("D-Pad Up pressed\n");
            y_pos -= move_step;
        }
        else if (strstr(buffer, "Dpad_Left")) {
            printf("D-Pad Left pressed\n");
            x_pos -= move_step;
        }
        else if (strstr(buffer, "Dpad_Right")) {
            printf("D-Pad Right pressed\n");
            x_pos += move_step;
        }else if (strstr(buffer, "A")) {
            printf("A Right pressed\n");
            left_click();
        }else if (strstr(buffer, "B")) {
            printf("A Right pressed\n");
            right_click();
        }else if(strstr(buffer, "1")){
            printf("1 pressed\n");
            page_up();
        }else if(strstr(buffer, "2")){
            printf("2 pressed");
            page_down();
        }else if(strstr(buffer, "Plus")){
            printf("Plus pressed\n");
            move_step += 5;
        }else if(strstr(buffer, "Minus")){
            printf("Minus pressed");
            move_step -= 5;
        }
        send_mouse_move(x_pos,y_pos);

        usleep(100000);  // Sleep for 100ms to avoid overloading CPU
    }

    close(fd);
    return 0;
}