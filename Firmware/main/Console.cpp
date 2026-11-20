#include "Console.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "Config.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"


//Good article on escape character in AINSI:
// https://medium.com/@fulton_shaun/brighten-up-your-terminal-with-ansi-codes-cb5f5d2ff085

#define CHAR_LENGTH 20
#define HISTORY_LENGTH 10

#define CONSOLE_UART ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)

static char readString[CHAR_LENGTH];
static char History[HISTORY_LENGTH][CHAR_LENGTH];

void console_init(void)
{
    // UART driver (115200 bauds, set by CONFIG_ESP_CONSOLE_UART_BAUDRATE) for the non-blocking reads
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONSOLE_UART);
}

int console_read(uint32_t timeout_ms)
{
    uint8_t c;
    int n = uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(timeout_ms));
    return (n == 1) ? c : -1;
}

// Variable globale pour mémoriser quelle commande exécuter
int last_cmd_index = -1;

command_t console_getInput(char input_char, char * argument, const console_t * console, int * cmd_index)
{
    static int8_t HistoryCounter = FIRST_TIME;
    int posy;
    
    command_t cmd = CMD_noCommand;
    * cmd_index = -1; // Reset à chaque appel

    //we read the first character
    //char input_char = uart_read(UART_CONSOLE);

    switch ((uint8_t)input_char)
    {
      case 13: 
      {//Si on appui sur "enter"
        //On veut que la dernière commande soit à la posion 0
        //Donc on décale tout le monde 1
        for (int i = (HISTORY_LENGTH - 1); i > 0; i--){
          strcpy(History[i],History[i-1]);
        }

        //On met la nouvelle valeur dans history
        strcpy(History[0],readString);

        //On imprime sur le port série 1 retour de ligne
        printf("\r\n");

        //*************  New parser   ***************
        // On cherche l'index de la commande dans le tableau
        * cmd_index = -1;
        
        // Séparation argument simple (cherche l'espace)
        char temp_read[50];
        strcpy(temp_read, readString);
        char *space_ptr = strchr(temp_read, ' ');

        if (space_ptr != NULL) {
            *space_ptr = '\0';
            strcpy(argument, space_ptr + 1);
        } else {
            strcpy(argument, "");
        }

        // Parcours du tableau pour trouver la fonction
        for (int i = 0; console[i].parser != NULL; i++) {
            if (strcmp(temp_read, console[i].parser) == 0) 
            {
                * cmd_index = i;
                cmd = CMD_noCommand; //
                break;
            }
        }
        
        if(* cmd_index == -1){ cmd = Error;} // Non trouvé
        
        //***************************************

        strcpy(readString,"");
        //Le seul moment où l'on ne remet pas le historycounter à FIRSTTIME
        // c'est si on a appuyé sur une flèche
        HistoryCounter = FIRST_TIME;
      break;
      }

      case 8: //Si on fait un backspace
        printf("\b \b");
        //on elève un caractère de readString
        if (strlen(readString) > 0) readString[strlen(readString)-1] = '\0';
        cmd = CMD_noCommand;
        //Le seul moment où l'on ne remet pas le historycounter à 0
        // c'est si on a appuyé sur une flèche
        HistoryCounter = FIRST_TIME;
      break;

      case 10:
        cmd = CMD_noCommand;
      break;

      case 27: //an arrow!!  we need to handle 3 caracters
        //On attend (max 5 ms) que les autres caractères aient eu le temps de rentrer dans le buffer
        //Si ce n'est pas un '['
        if (console_read(5)!=91){
            uart_flush_input(CONSOLE_UART);   // on jette le reste de la séquence
            cmd = Error;
            break;
        }
        //Si le 3e caractère est un 'A' c'est que c'est la flèche du haut, 'B' par en bas
        switch (console_read(5))
        {
            case 65:
              //Serial.println("flèche up!!");
              if (HistoryCounter == FIRST_TIME){
                HistoryCounter = 0;
              }
              else if (HistoryCounter <= 8){
                HistoryCounter++;
              }
              printf("\x1b[2K\r> ");
              printf(BGRN "%s" AINSIrst,History[HistoryCounter]);
              strcpy(readString,History[HistoryCounter]);
              cmd = CMD_noCommand;
            break;

            case 66:
              //Serial.println("flèche Down!!");
              if (HistoryCounter == FIRST_TIME){
                HistoryCounter = 0;
              }
              else if (HistoryCounter >= 1){
                HistoryCounter--;
              }
              printf("\x1b[2K\r> ");
              printf(BGRN "%s" AINSIrst,History[HistoryCounter]);
              strcpy(readString,History[HistoryCounter]);
              cmd = CMD_noCommand;
            break;

            default:
              cmd = Error;
            break;
        }

      break;

      case 32 ... 127:  //les caractère standard
        //Add to string
        posy = strlen(readString); 
        readString[posy] = input_char;
        readString[posy + 1] = '\0';
  
        //Echo

        //uart_write(USART_SERIAL, c);
        printf(BGRN "%c" AINSIrst,input_char);
               
        //Le seul moment où l'on ne remet pas le historycounter à 0
        // c'est si on a appuyé sur une flèche
        HistoryCounter = FIRST_TIME;
        cmd = CMD_noCommand;
      break;

      default:
        //Le seul moment où l'on ne remet pas le historycounter à 0
        // c'est si on a appuyé sur une flèche
        HistoryCounter = FIRST_TIME;
        cmd = Error;
      break;
    }
    //On vide le buffer d'entrée au ca où il y aurait encore dequoi dedans
    //fflush(stdin);
    //uart_empty_rx_buf();

    return cmd;
}



static void print_colored_stars(int count) {
    for (int i = 0; i < count; i++) {
        // Obtenir un index de couleur dynamique dans le cube de couleur RGB (16 à 231)
        // console_random() renvoie un int32_t (peut être négatif), 
        // donc on utilise abs() pour garantir un index positif avant le modulo.
        int color_index = (abs(console_random()) % 216) + 16; 
        
        // Séquence d'échappement ANSI étendue pour le texte (38;5;<index>)
        printf("\033[38;5;%dm*", color_index);
    }
    printf(AINSIrst); // "\033[0m" Réinitialise la couleur à blanc à la fin de la ligne
}

void console_displayHistory(){
    print_colored_stars(5);
    printf(" History: ");
    print_colored_stars(5);
    printf("\r\n");

    for (int i = 0; i < 10; i++){
        if(strlen(History[i]) == 0)
          {break;}
        printf("%s",History[i]);
        printf("\r\n");
    }
    printf("\r\n");
}

void console_displayIntro(){
  printf("\033[2J\033[H"); //clear the screen and put to cursor at home
  print_colored_stars(57); printf("\r\n");
  printf(    "   Use Tera Term in ANSI mode to support up/down arrows \r\n");
  printf(    "           Look nice with Consolas Font in 14           \r\n");
  printf(    "        Type 'help' to display all the commands         \r\n");
  printf(  "                  ESP32-WROOM-32E-N8R2                    \r\n");
  print_colored_stars(57); printf("\r\n> ");
}

void console_displayHelp(console_t * console){
  for (int i = 0; console[i].parser != NULL; i++) {
    /* 
        %-12s :
        - le '-' aligne le texte à GAUCHE.
        - le '12' réserve exactement 12 caractères de large.
        - le 's' indique qu'on affiche une chaîne (string).
    */
    if (console[i].nb_arg == 0){
      printf("%-14s : %s\r\n", console[i].parser, console[i].help);
    }
    else {
      printf("%-12s x : %s\r\n", console[i].parser, console[i].help);
    }
  }
}

void console_test(void)
{
  print_colored_stars(57); printf("\r\n");
  print_colored_stars(57); printf("\r\n");
  print_colored_stars(57); printf("\r\n");
  printf(BRED "       Hey this is the color red, and it's bold! \r\n" AINSIrst);
	printf(RED  "               If " BLU " you" YEL " are" GRN " bored" CYN " do" MAG " this! \r\n" AINSIrst); 
	printf(BRED "               If " BBLU " you" BYEL " are" BGRN " bored" BCYN " do" BMAG " this! \r\n" AINSIrst); 
	printf(URED "               If " UBLU " you" UYEL " are" UGRN " bored" UCYN " do" UMAG " this! \r\n" AINSIrst);
  print_colored_stars(57); printf("\r\n");
  print_colored_stars(57); printf("\r\n");
  print_colored_stars(57); printf("\r\n");
}