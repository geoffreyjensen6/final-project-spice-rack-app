#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

int read_weight(char *read_val, int read_len){
	int hx711_fd;
	int count = 0;
	int result = 0;
	char file_name[] = "/sys/bus/iio/devices/iio:device0/in_voltage0_raw";
	
	hx711_fd = open(file_name, O_RDONLY);
	if(hx711_fd == -1){
		perror("Spice_Rack_App: Failed to Open HX711 File - ");
		return -1;
	}
	while(read_len != 0 && (count = read(hx711_fd, read_val, read_len)) != 0){
		if(count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: Reading weight failed - ");
			result = -1;
			break;
		}
		read_len = read_len - count;
	}
	syslog(LOG_DEBUG,"Spice_Rack_App: read_weight - Weight Reading is %s", read_val);
	close(hx711_fd);
	return result;
}

int read_fsr_status(){
	int result = 0;
	int fsr_fd;
	int count = 0;
	char file_name[] = "/dev/fsr_gpio_0";
	unsigned char read_val;
	
	fsr_fd = open(file_name, O_RDONLY);
	if(fsr_fd == -1){
		perror("Spice_Rack_App: Failed to Open FSR Device File - ");
		return -1;
	}
	while((count = read(fsr_fd, &read_val, 1)) != 1){
		if(count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: Reading FSR status failed - ");
			result = -1;
			break;
		}
	}
	syslog(LOG_DEBUG,"Spice_Rack_App: read_fsr_status - FSR Reading is %d", read_val);
	result = read_val;
	return result;
}

int main() {
	char *read_val;
	int read_len = 16;
	int i = 0;
	int fsr_status;

	openlog(NULL,0,LOG_USER);
	syslog(LOG_DEBUG,"Spice_Rack_App: Starting Application");

		
	read_val = (char *)malloc(read_len * sizeof(char));
	for(i=0; i<20; i++){
		memset(read_val,0,read_len);
		fsr_status = read_fsr_status();
		syslog(LOG_DEBUG,"Spice_Rack_App: main - FSR Status is %i", fsr_status);
		printf("FSR Status is %i\n", fsr_status);
		read_weight(read_val, read_len);
		syslog(LOG_DEBUG,"Spice_Rack_App: main - Weight Reading is %s", read_val);
		printf("Weight Reading is %s", read_val);
	}
	free(read_val);

}
