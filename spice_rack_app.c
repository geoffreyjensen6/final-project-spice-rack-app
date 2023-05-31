#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int read_weight(){
	int hx711_fd;
	int count;
	char *read_val;
	char file_name[] = "/sys/bus/iio/devices/iio:device0/in_voltage0_raw";
	
	hx711_fd = open(file_name, O_RDONLY);
	if(hx711_fd == -1){
		perror("Spice_Rack_App: Failed to Open HX711 File - ");
		printf("File name is %s\n", file_name);
		return -1;
	}
	read_val = (char *)malloc(sizeof(char));
	count = read(hx711_fd, &read_val, 1);
	if(count == -1){
		perror("Spice_Rack_App: Failed on Read of HX711 File - ");
		return -1;
	}
	printf("Weight Reading is %s\n", read_val);
	free(read_val);
	close(hx711_fd);
	return 0;
}

int main() {
	int fd = 0;
	int count = 0;
	unsigned char c;
	int i = 0;

	fd = open("/dev/fsr_gpio_0", O_RDONLY);
	if(fd == -1){
		perror("fsr_open failed");
	}
	count = read(fd, &c, 1);
	if(count == -1){
		perror("fsr_read failed");
	}
	printf("GPIO value is %d\n", c);
	
	for(i=0; i<20; i++){
		read_weight();
	}
}
