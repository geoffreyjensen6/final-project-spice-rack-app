#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <limits.h>

#define MAX_SPICE_NUM_DIGITS 2
#define MAX_SPICE_NAME_SIZE 32
#define CALIBRATE_BTN_VAL_FILE "/sys/class/gpio/gpio27/value"
#define CALIBRATE_BTN_DIR_FILE "/sys/class/gpio/gpio27/direction"
#define CALIBRATE_GPIO "27"
#define HX711_FILE "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define FSR_FILE "/dev/fsr_gpio_0"
#define OUTPUT_FILE "/var/tmp/spice_rack_measurements.txt"

int empty_jar_mass = 128;

//Finding the file size in order to malloc appropriately sized char *.
off_t find_file_size(char *file_name){
	int fd = 0;
	off_t file_length = 0;

	fd = open(file_name, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: find_file_size - Failed to Open File - ");
		return -1;
	}

	file_length = lseek(fd, 0, SEEK_END);
	syslog(LOG_DEBUG, "Spice_Rack_App: find_file_size - file_length = %li\n", file_length);
	if(file_length  == -1){
		perror("Spice_Rack_App: find_file_size - Seeking to end of file failed - ");
		return -1;
	}

	close(fd);
	return file_length;
}

//Read Entire file contents into char *. Used for parsing through the input file conents
int read_file(char *file_name, char *output_buf, off_t read_len){
	int fd = 0;
	off_t count = 0;
	int result = 0;
	
	fd = open(file_name, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: read_file - Failed to Open File - ");
		return -1;
	}
	
	while(read_len != 0 && (count = read(fd, output_buf, read_len)) != 0){
		if(count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: Reading File failed - ");
			result = -1;
			break;
		}
		read_len = read_len - count;
	}
	
	close(fd);
	return result;
}

//Used to store measurement data to a file. Handles both creating for the first time as well as updating 
//data for individual spices. 
int store_measurement(int spice_num, char *spice_name, char *weight){
	int input_fd;
	int output_fd;
	int output_str_len;
	int count;
	int result = 0;
	int need_to_insert = 0;	
	off_t file_length = 0;
	off_t match_offset = 0;
	off_t eol_offset = 0;
	off_t write_len = 0;
	char *spice_num_str;
	char *match_str;
	char *eol;
	char *read_buff;
	char file_name[] = OUTPUT_FILE;
	char *output_str;
	
	
	//Open file for RD/WR and create if it doesn't already exist. 
	input_fd = open(file_name, O_CREAT | O_RDWR);
	if(input_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open File - ");
		return -1;
	}
	
	//Find File Size
	file_length = find_file_size(file_name);	
	syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - File length is %li\n", file_length);
	
	//Allocate Read Buffer which will hold the current file contents if there are any.
	read_buff = (char *)malloc(file_length * sizeof(char));
	memset(read_buff,0,file_length);

	//Read complete file into buffer
	if(read_file(file_name, read_buff, file_length) == -1){
		perror("Spice_Rack_App: store_measurement - Failed to read file contents to read buffer - ");
		return -1;
	}

	//Close input file now that contents were all read out. Going to repoen this file and truncate 
	//because often measurement changes could be fewer characters and lead to extra whitespace
	//and newlines at the EOF. So it is needed to write whole file again to avoid that.
	close(input_fd);
	//Open file for writing and create if it doesn't already exist and truncate because we will rewrite
	output_fd = open(file_name, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if(output_fd == -1){
		perror("Spice_Rack_App: store_measurement - Failed to Open File - ");
		return -1;
	}			
	
	//Generate the new output string
	//Output format is: Spice#	Spice_Name	Weight
	//TODO Still need to incorporate the spice_name into length and format for output_string
	if((strstr(spice_name, "Empty Jar") != NULL) || (strcmp(spice_name, "Empty Rack") == 0)){
		spice_num_str = (char *)malloc(15 * sizeof(char));
		memset(spice_num_str,0,strlen(spice_num_str));
		sprintf(spice_num_str, "N/A-%s", spice_name);
	} 
	else{
		spice_num_str = (char *)malloc((MAX_SPICE_NUM_DIGITS+6) * sizeof(char));
		memset(spice_num_str,0,strlen(spice_num_str));
		sprintf(spice_num_str, "Spice%i", spice_num);
	}
	output_str_len = strlen(spice_num_str) + strlen(spice_name) + strlen(weight) + 16;
	output_str = (char *)malloc(output_str_len * sizeof(char));
	memset(output_str,0,output_str_len);
	sprintf(output_str, "%s\t\tSpice_Name:%s\t%s\n", spice_num_str, spice_name, weight);
	syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - output_str_len is %i and output_str is %s\n", output_str_len, output_str);

	//Check if Spice# already has an entry in the file
	//If entry exists, seek to it. If doesn't exist, seek to EOF
	match_str = strstr(read_buff, spice_num_str);
	if(match_str != NULL){
		match_offset = match_str - read_buff;
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Found a location in file where %s already exists. Overwriting and shifting data", spice_num_str);
		need_to_insert = 1;
		//Write all previous content up until the match location to the output file.
		count = write(output_fd, read_buff, match_offset);
		if(count == -1){
			perror("Spice_Rack_App: Writing up to match location to File failed - ");
			result = -1;
		}
		//If found entry, determine where the end of the line is for the entry in the current file. 
		//We will overwrite it and the string size may change so we will need to shift all later data.
		eol = strchr(match_str, '\n');
		eol_offset = eol - match_str + 1;
		if(eol_offset != output_str_len){
			syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - line size changed. Need to shift all later data\n");
		}
	}
	else{
		//Write all previous content up until the EOF to the output file.
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - %s entry doesn't exist in output file. Will append to the end", spice_num_str);
		count = write(output_fd, read_buff, file_length);
		if(count == -1){
			perror("Spice_Rack_App: Writing all data to File failed - ");
			result = -1;
		}
	}

	//Write new Entry
	syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Writing output_str=%s to file", output_str);
	count = write(output_fd, output_str, strlen(output_str));
	if(count == -1){
		perror("Spice_Rack_App: Writing Measurements to File failed - ");
		result = -1;
	}
	
	//Check if need_to_insert = 1 and then write remainder of file adjusted.
	if(need_to_insert == 1){
		match_str = eol_offset + match_str;
		write_len = file_length - (match_str - read_buff);
		count = write(output_fd, match_str, write_len);
		if(count == -1){
			perror("Spice_Rack_App: Writing Measurements to File failed - ");
			result = -1;
		}
	}		
	close(output_fd);
	free(spice_num_str);
	syslog(LOG_DEBUG, "Made it Here\n");
	free(read_buff);
	syslog(LOG_DEBUG, "Made it Here\n");
	free(output_str);
	syslog(LOG_DEBUG, "Made it Here\n");

	return result;
}


int read_weight(char *read_val, int read_len){
	int hx711_fd;
	int count = 0;
	int result = 0;
	char *temp;
	
	hx711_fd = open(HX711_FILE, O_RDONLY);
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
	
	//Remove trailing newline character from string
	temp = strchr(read_val, '\n');
	if(temp != NULL){
		*temp = '\0';
	}

	syslog(LOG_DEBUG,"Spice_Rack_App: read_weight - Weight Reading is %s", read_val);
	close(hx711_fd);
	return result;
}

int get_average_weight(char *read_val, int read_len, int sample_num){
	int i;
	int sample_val = 0;
	long long sample_total = 0;
	int sample_average = 0;
	char *end_ptr;

	for(i=0; i<sample_num; i++){
		read_weight(read_val, read_len);
		sample_val = strtol(read_val, &end_ptr, 10);
		syslog(LOG_DEBUG, "Sample value is %i\n", sample_val);
		sample_total = sample_total + sample_val;
	}
	sample_average = sample_total/sample_num;
	syslog(LOG_DEBUG, "Sample Average is %i\n", sample_average);
	return sample_average;
}

int read_fsr_status(){
	int result = 0;
	int fsr_fd;
	int count = 0;
	int i = 0;
	int debounce_count = 0;
	unsigned char read_val;
	
	fsr_fd = open(FSR_FILE, O_RDONLY);
	if(fsr_fd == -1){
		perror("Spice_Rack_App: Failed to Open FSR Device File - ");
		return -1;
	}

	//Loop to debounce the FSR sensor readings to ensure if we detect a change, it remains changed. 
	//Needed to extend to debounce for ~2 seconds because it is not only the FSR reading we want to 
	//settle but also the Weight sensor must settle too. Took measurements to see about how long it 
	//took for it to settle and added some guardband.
	while(i<10 && debounce_count<10){
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
		if(i == 0){
			result = read_val;
		}
		usleep(200000);
		i++;
		if(result == read_val){
			debounce_count++;
		}
	}
	if(debounce_count != 10){
		result = -1;
	}
	close(fsr_fd);
	return result;
}

int read_calibrate_button(){
	int fd;
	int result;
	char read_val;

	fd = open(CALIBRATE_BTN_VAL_FILE, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: Failed to /sys/class/gpio/gpioX/value - ");
		return -1;
	}
	result = read(fd, &read_val, 1);
	if(result != 1){
		perror("Spice_Rack_App: Failed to read to /sys/class/gpio/gpioX/value - ");
		return -1;
	}
	close(fd);
	syslog(LOG_DEBUG,"Spice_Rack_App: read_calibrate_button - Calibrate Button is %d", read_val);
	result = atoi(&read_val);
	return result;
}

int calibrate_spice_rack(char *read_val, int read_len){
	int fsr_status;
	int prev_fsr_status = 0;
	int fsr_diff = 0;
	int spice_num = 0;
	char *user_input_val;
	char *end_ptr;
	char *spice_name;

	spice_name = (char *)malloc(MAX_SPICE_NAME_SIZE * sizeof(char));
	memset(spice_name,0,MAX_SPICE_NAME_SIZE);

	printf("Please remove all spices from Spice Rack to begin calibration\n");
	while((fsr_status = read_fsr_status()) != 0){
		sleep(1);
	}
	printf("All spices have been removed. Collecting weight measurement of empty rack\n");
	get_average_weight(read_val, read_len,10);
	syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Empty Rack Weight Reading is %s", read_val);
	printf("Empty Rack Weight Reading is %s\n", read_val);
	strcpy(spice_name, "Empty Rack");
	store_measurement(spice_num, spice_name, read_val);

	printf("\nAn Empty Spice Jar is assumed to be %i Grams.\n", empty_jar_mass);
	printf("Do you wish to change this? (y/n)?)");
	user_input_val = (char *)malloc(10 * sizeof(char));
	while(1){
		memset(user_input_val,0,10);
		if(fgets(user_input_val, 10, stdin)){
			user_input_val[strcspn(user_input_val, "\n")] = 0;
		}
		if(strcmp(user_input_val,"y") == 0){
			printf("Selected y\n");
			while(1){
				printf("Enter the mass in grams that you wish to use. (Example: 128): ");
				memset(user_input_val,0,10);
				if(fgets(user_input_val, 10, stdin)){
					user_input_val[strcspn(user_input_val, "\n")] = 0;
				}
				empty_jar_mass = strtol(user_input_val, &end_ptr, 10);
				if(empty_jar_mass == LONG_MIN || empty_jar_mass == LONG_MAX){
					perror("Spice_Rack_App: calibrate_spice_rack - User entered mass that is invalid - ");
					continue;
				}
				break;
			}
			break;
		}
		else if(strcmp(user_input_val, "n") == 0){
			break;
		}
		else{
			printf("Invalid Entry. Please enter y or n\n");
			sleep(5);
		}
	}
	free(user_input_val);	

	printf("Place an empty jar on the spice rack now in spice1 position\n");
	while(1){
		fsr_status = read_fsr_status();
		if(fsr_status != 1){
			continue;
		}
		get_average_weight(read_val, read_len, 10);
		syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Empty Jar Weight Reading is %s", read_val);
		printf("Empty Jar Weight Reading is %s\n", read_val);
		memset(spice_name,0,MAX_SPICE_NAME_SIZE);
		sprintf(spice_name, "Empty Jar-%ig", empty_jar_mass);
		store_measurement(spice_num, spice_name, read_val);
		break;
	}

	printf("Please remove empty jar from Spice1 location now\n");
	while(fsr_status != 0){
		fsr_status = read_fsr_status();
	}

	printf("Now you will need to place and leave each spice on the rack. Only place one spice at a time when prompted to do so.\nGo ahead and place the first spice in Spice1 position\n");
	while(1){
		spice_num = 1;
		fsr_status = read_fsr_status();
		if(fsr_status != prev_fsr_status){
			fsr_diff = fsr_status - prev_fsr_status;
			if(fsr_diff < 0){
				continue;
			}
			while(fsr_diff != 1){
				spice_num = spice_num + 1;
				fsr_diff = fsr_diff >> 1;
			}
			printf("Detected a spice was placed in Spice%i position. Beginning weighing now\n", spice_num);
			get_average_weight(read_val, read_len,10);
			syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Weight Reading for Spice%i is %s", spice_num, read_val);
			printf("Spice%i Weight Reading is %s\n", spice_num, read_val);
			//TODO Add scanf function to have user input spice names and include that in call to store
			memset(spice_name,0,MAX_SPICE_NAME_SIZE);
			printf("Enter the name of this spice: ");
			if(fgets(spice_name, MAX_SPICE_NAME_SIZE, stdin)){
				spice_name[strcspn(spice_name, "\n")] = 0;
				printf("Spice Name is %s\n", spice_name);
			}
			store_measurement(spice_num, spice_name, read_val);
			prev_fsr_status = fsr_status;
			if(spice_num == 3){
				break;
			}
			else{
				printf("Place the next desired spice on the Spice Rack now\n");
			}

		}
	}
	free(spice_name);
	return 0;
}

int setup_calibrate_button(){
	int fd;
	int result;

	//Export GPIO (calibrate button)
	fd = open("/sys/class/gpio/export", O_WRONLY);
	if(fd == -1){
		perror("Spice_Rack_App: Failed to /sys/class/gpio/export - ");
		return -1;
	}
	result = write(fd, CALIBRATE_GPIO, 2);
	if(result != 2){
		perror("Spice_Rack_App: Failed to write to /sys/class/gpio/export - ");
		return -1;
	}
	close(fd);
	
	//Set direction of GPIO to "in"
	fd = open(CALIBRATE_BTN_DIR_FILE, O_WRONLY);
	if(fd == -1){
		perror("Spice_Rack_App: Failed to /sys/class/gpio/gpio27/direction - ");
		return -1;
	}
	result = write(fd, "in", 2);
	if(result != 2){
		perror("Spice_Rack_App: Failed to write to /sys/class/gpio/gpio27/direction - ");
		return -1;
	}
	close(fd);
	return 0;
}

int free_calibrate_button(){
	int fd;
	int result;

	//Unexport GPIO (calibrate button)
	fd = open("/sys/class/gpio/unexport", O_WRONLY);
	if(fd == -1){
		perror("Spice_Rack_App: Failed to /sys/class/gpio/unexport - ");
		return -1;
	}
	result = write(fd, CALIBRATE_GPIO, 2);
	if(result != 2){
		perror("Spice_Rack_App: Failed to write to /sys/class/gpio/unexport - ");
		return -1;
	}
	close(fd);
	return 0;
}

int main() {
	char *read_val;
	int read_len = 16;
	int i = 0;
	int fsr_status;
	int calibrate = 0;
	int fd;

	openlog(NULL,0,LOG_USER);
	syslog(LOG_DEBUG,"Spice_Rack_App: Starting Application");

	setup_calibrate_button();
	read_val = (char *)malloc(read_len * sizeof(char));
	memset(read_val,0,read_len);

	fd = open(OUTPUT_FILE, O_RDONLY);
	if(fd == -1){
		printf("Unable to find previous calibration data to use. Performing a new calibration\n");
		calibrate_spice_rack(read_val, read_len);
	}
	else{
		printf("Found previous calibration data to use. To perform new calibration press the calibration button\n");
	}
	get_average_weight(read_val, read_len, 10);
	for(i=0; i<20; i++){
		calibrate = read_calibrate_button();
		syslog(LOG_DEBUG,"Spice_Rack_App: main - Calibrate Button is %i", calibrate);
		printf("Calibrate Button is %i\n", calibrate);

		memset(read_val,0,read_len);
		fsr_status = read_fsr_status();
		syslog(LOG_DEBUG,"Spice_Rack_App: main - FSR Status is %i", fsr_status);
		printf("FSR Status is %i\n", fsr_status);

		read_weight(read_val, read_len);
		syslog(LOG_DEBUG,"Spice_Rack_App: main - Weight Reading is %s", read_val);
		printf("Weight Reading is %s\n", read_val);
	}
	free(read_val);
	free_calibrate_button();
}
