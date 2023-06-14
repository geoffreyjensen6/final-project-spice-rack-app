#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include "spice_rack_app.h"

#define MAX_SPICE_NUM_DIGITS 2
#define MAX_SPICE_NAME_SIZE 32
#define SPICE_RACK_SIZE 3
#define NUM_COLUMNS 4
#define EMPTY_JAR_MASS_DEF 133.245
#define CALIBRATE_BTN_VAL_FILE "/sys/class/gpio/gpio27/value"
#define CALIBRATE_BTN_DIR_FILE "/sys/class/gpio/gpio27/direction"
#define CALIBRATE_GPIO "27"
#define HX711_FILE "/sys/bus/iio/devices/iio:device0/in_voltage0_raw"
#define FSR_FILE "/dev/fsr_gpio_0"
#define OUTPUT_FILE "/var/tmp/spice_rack_measurements.txt"

struct spice_rack *spice_rack;

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

int copy_file_contents(int in_fd, int out_fd, off_t end_location){
	int result = 0;
	off_t rd_count = 0;
	off_t wr_count = 0;
	off_t read_len = 1;
	off_t curr_position;
	char next_char[1];
	
	curr_position = lseek(in_fd, 0, SEEK_SET);
	if(curr_position  == -1){
		perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
		return -1;
	}

	if(end_location != 0){
		read_len = end_location - curr_position;
	}
	else{
		read_len = 1;
	}
	while((rd_count = read(in_fd, next_char, 1)) != 0 && (read_len != 0)){
		if(rd_count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: Reading File failed - ");
			result = -1;
			break;
		}
		if(*next_char == EOF){
			printf("Found EOF");
			break;
		}
		read_len = read_len - rd_count;
		wr_count = write(out_fd, next_char, 1);
		if(wr_count == -1){
			perror("Spice_Rack_App: Writing Measurements to File failed - ");
			result = -1;
		}
	}
		
	return result;
}

int copy_file(int in_fd, int out_fd, off_t end_location){
	int result = 0;
	off_t rd_count = 0;
	off_t wr_count = 0;
	off_t read_len = 1;
	off_t curr_position;
	char next_char[1];
	
	curr_position = lseek(in_fd, 0, SEEK_CUR);
	if(curr_position  == -1){
		perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
		return -1;
	}

	if(end_location != 0){
		read_len = end_location - curr_position;
	}
	else{
		read_len = 1;
	}
	while((rd_count = read(in_fd, next_char, 1)) != 0 && (read_len != 0)){
		if(rd_count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: Reading File failed - ");
			result = -1;
			break;
		}
		if(*next_char == EOF){
			printf("Found EOF");
			break;
		}
		read_len = read_len - rd_count;
		wr_count = write(out_fd, next_char, 1);
		if(wr_count == -1){
			perror("Spice_Rack_App: Writing Measurements to File failed - ");
			result = -1;
		}
	}
		
	return result;
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
		printf("read_len = %li and count = %li\n", read_len, count);
	}
	printf("read_buf in read_file is :%s\n", output_buf);
	
	close(fd);
	return result;
}

int read_line(int fd, char *output_str){
	int count;
	int result = 0;
	char *read_buff;
	read_buff = (char *)malloc(1 * sizeof(char));
	memset(output_str,0,strlen(output_str));

	while((count = read(fd, read_buff, 1)) != 0){
		if(count == -1){
			if(errno == EINTR){
				continue;
			}
			perror("Spice_Rack_App: read_line - Reading File failed - ");
			result = -1;
			break;
		}
		if(strchr(read_buff, '\n') != NULL){
			printf("output_str is %s\n", output_str);
			break;
		}
		strcat(output_str, read_buff);
	}
	
	free(read_buff);
	return result;
}

int parse_line(char *output_str, int i){
	int result = 0;
	int substring_len = 0;
	char *start_ptr = output_str;
	char *comma_ptr;
	int j;

	for(j=0;j<NUM_COLUMNS;j++){
		if((start_ptr = strstr(output_str, spice_rack->spices[i].spice_search_strings.search_strings[j])) != NULL){
			start_ptr = start_ptr + strlen(spice_rack->spices[i].spice_search_strings.search_strings[j]);
		}
		if((comma_ptr = strchr(start_ptr, ',')) != NULL){
			substring_len = comma_ptr - start_ptr + 1;
			snprintf(spice_rack->spices[i].spice_entries.entries[j], substring_len, "%s", start_ptr);
			start_ptr = output_str + strlen(spice_rack->spices[i].spice_search_strings.search_strings[j]) + substring_len;
		}
		//printf("Spice search string=%s and entry=%s\n", spice_rack->spices[i].spice_search_strings.search_strings[j], spice_rack->spices[i].spice_entries.entries[j]);
	}
	return result;
}

off_t search_file(int in_fd, char *search_term){
	int i = 0;
	off_t file_offset;
	off_t end_of_line = 0;
	char read_line_buff[120];
	char *match_str;

	//set position to beginning of file
	file_offset = lseek(in_fd, 0, SEEK_SET);
	if(file_offset  == -1){
		perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
		return -1;
	}
	
	//Search through each line for the search term and stop on the line that you find it
	for(i=1;i<=(SPICE_RACK_SIZE+2);i++){
		//Grab file offset at beginning of line so we can restore it if we find a match
		file_offset = lseek(in_fd, 0, SEEK_CUR);
		if(file_offset  == -1){
			perror("Spice_Rack_App: search_file - Seeking in file failed - ");
			return -1;
		}
		printf("Current offset is %jd\n", file_offset);
		read_line(in_fd, read_line_buff);
		printf("Read Line is %s\n", read_line_buff);
		if((match_str = strstr(read_line_buff, search_term)) != NULL){
			printf("Found a match at line %i\n", i);
			end_of_line = lseek(in_fd, 0, SEEK_CUR);
			//Restore file offset to beginning of this line
			file_offset = lseek(in_fd, file_offset, SEEK_SET);
			printf("Restored file offset to %jd and end_of_line is %jd\n", file_offset, end_of_line);
			if(file_offset  == -1){
				perror("Spice_Rack_App: search_file - Seeking in file failed - ");
				return -1;
			}
			break;
		}		
	}	

	return end_of_line;
}

//Used to store measurement data to a file. Handles both creating for the first time as well as updating 
//data for individual spices. 
int store_measurement(int spice_num, char *spice_name, char *weight, float mass){
	int input_fd;
	int temp_fd;
	int output_fd;
	int output_str_len;
	int count;
	int result = 0;
	off_t file_length = 0;
	off_t file_offset = 0;
	off_t eol;
	off_t match_offset = 0;
	char *spice_num_str;
	char file_name[] = OUTPUT_FILE;
	char *output_format_str;
	
	
	//Open file for RD/WR and create if it doesn't already exist. 
	input_fd = open(file_name, O_CREAT | O_RDWR);
	if(input_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open File - ");
		return -1;
	}
	temp_fd = open("/var/log/spice_rack_tmp.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
	if(temp_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open File - ");
		return -1;
	}
	
	//Find File Size
	file_length = find_file_size(file_name);
	syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - File length is %li\n", file_length);

	if(copy_file(input_fd, temp_fd, EOF) == -1){
		perror("Spice_Rack_App: store_measurement - Failed to read file contents to read buffer - ");
		return -1;
	}

	//Close input file now that contents were all read out. Going to repoen this file and truncate 
	//because often measurement changes could be fewer characters and lead to extra whitespace
	//and newlines at the EOF. So it is needed to write whole file again to avoid that.
	close(temp_fd);
	close(input_fd);
	temp_fd = open("/var/log/spice_rack_tmp.txt", O_RDONLY);
	if(temp_fd == -1){
		perror("Spice_Rack_App: store_measurements - Failed to Open File - ");
		return -1;
	}
	//Open file for writing and create if it doesn't already exist and truncate because we will rewrite
	output_fd = open(file_name, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if(output_fd == -1){
		perror("Spice_Rack_App: store_measurement - Failed to Open File - ");
		return -1;
	}			
	
	//Generate the new output string
	//Output format is: Spice#	Spice_Name	Weight
	if((strstr(spice_name, "Empty Jar") != NULL) || (strcmp(spice_name, "Empty Rack") == 0)){
		spice_num_str = (char *)malloc(32 * sizeof(char));
		if(spice_num_str == NULL){
			perror("Spice_Rack_App: store_measurement - Couldn't allocate memory - ");
		}
		memset(spice_num_str,0,strlen(spice_num_str));
		snprintf(spice_num_str,32,"N/A-%s", spice_name);
	} 
	else{
		spice_num_str = (char *)malloc(8 * sizeof(char));
		if(spice_num_str == NULL){
			perror("Spice_Rack_App: store_measurement - Couldn't allocate memory - ");
		}
		memset(spice_num_str,0,strlen(spice_num_str));
		snprintf(spice_num_str,7,"Spice%i", spice_num);
	}
	output_str_len = 120;
	output_format_str = (char *)malloc(output_str_len * sizeof(char));
	if(output_format_str == NULL){
		perror("Spice_Rack_App: store_measurement - Couldn't allocate memory - ");
		return -1;
	}
	memset(output_format_str,0,output_str_len);
	snprintf(output_format_str, (output_str_len-1), "Spice_Location:%s,Spice_Name:%s,ADC_Reading:%s,Calibrated_Weight(grams):%3.6f\n", spice_num_str, spice_name, weight, mass);
	syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - output_str_len is %i and output_str is %s\n", output_str_len, output_format_str);


	if((eol = search_file(temp_fd, spice_num_str)) == 0){
		//Will be appending data to EOF
		printf("New Data. Need to append to EOF\n");
		//Reset position to beginning of file
		file_offset = lseek(temp_fd, 0, SEEK_SET);
		if(file_offset  == -1){
			perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
			return -1;
		}
		//copy existing file contents
		if(copy_file(temp_fd, output_fd, EOF) == -1){
			perror("Spice_Rack_App: store_measurement - Failed to read file contents to read buffer - ");
			return -1;
		}
		//Write new Entry
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Writing output_str=%s to file", output_format_str);
		count = write(output_fd, output_format_str, strlen(output_format_str));
		if(count == -1){
			perror("Spice_Rack_App: Writing Measurements to File failed - ");
			result = -1;
		}
	}
	else{
		//Will be inserting at current file position offset
		printf("Found existing Entry with same Spice Number. Replacing that Line\n");
		//Reset position to beginning of file
		match_offset = lseek(temp_fd, 0, SEEK_CUR);
		if(match_offset  == -1){
			perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
			return -1;
		}
		file_offset = lseek(temp_fd, 0, SEEK_SET);
		if(file_offset  == -1){
			perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
			return -1;
		}

		//copy existing file contents up to the match location
		if(match_offset > 0){
			if(copy_file(temp_fd, output_fd, match_offset) == -1){
				perror("Spice_Rack_App: store_measurement - Failed to read file contents to read buffer - ");
				return -1;
			}
		}

		//Write new Entry
		syslog(LOG_DEBUG, "Spice_Rack_App: store_measurement - Writing output_str=%s to file", output_format_str);
		count = write(output_fd, output_format_str, strlen(output_format_str));
		if(count == -1){
			perror("Spice_Rack_App: Writing Measurements to File failed - ");
			result = -1;
		}

		file_offset = lseek(temp_fd, eol, SEEK_SET);
		if(file_offset  == -1){
			perror("Spice_Rack_App: search_file - Seeking to beginning of file failed - ");
			return -1;
		}		

		//copy existing file contents from end of replaced line to EOF
		if(copy_file(temp_fd, output_fd, EOF) == -1){
			perror("Spice_Rack_App: store_measurement - Failed to read file contents to read buffer - ");
			return -1;
		}
	}	

	close(output_fd);
	close(temp_fd);
	free(output_format_str);
	free(spice_num_str);
	return result;
}


int read_weight(char *read_val, int read_len){
	int hx711_fd;
	int count = 0;
	int result = 0;
	
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

	//store previous adc reading in struct before collecting new ones
	spice_rack->previous_adc_reading = spice_rack->curr_adc_reading;

	for(i=0; i<sample_num; i++){
		read_weight(read_val, read_len);
		sample_val = strtol(read_val, &end_ptr, 10);
		syslog(LOG_DEBUG, "Sample value is %i\n", sample_val);
		sample_total = sample_total + sample_val;
	}
	sample_average = sample_total/sample_num;
	syslog(LOG_DEBUG, "Sample Average is %i\n", sample_average);
	snprintf(read_val, strlen(read_val), "%i", sample_average);
	spice_rack->curr_adc_reading = sample_average;
	return sample_average;
}

float adc_reading_to_grams(){
	float m = 0;
	int x = 0;
	float result;
	syslog(LOG_DEBUG, "Spice_Rack_App: adc_reading_to_grams - empty_jar_adc = %i, empty_rack_adc = %i, and empty_jar_mass = %f", spice_rack->empty_jar_adc, spice_rack->empty_rack_adc, spice_rack->empty_jar_mass);
	if(spice_rack->empty_jar_adc && spice_rack->empty_rack_adc){
		m = ((float)spice_rack->empty_jar_adc - spice_rack->empty_rack_adc)/spice_rack->empty_jar_mass;
	}
//TODO need to have a variable or struct that has Mutex access locks for previous weight and next weight. This way you can get Delta between the two and then calculate the actual adc_reading.
	if(spice_rack->curr_adc_reading > spice_rack->previous_adc_reading){
		x = spice_rack->curr_adc_reading - spice_rack->previous_adc_reading;
	}
	else{
		x = spice_rack->previous_adc_reading - spice_rack->curr_adc_reading;
	}

	syslog(LOG_DEBUG,"x=%i and m=%f\n", x, m);
	result = x/m - spice_rack->empty_jar_mass;
	printf("Result is %f grams\n", result);

	return result;
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
		if(result != read_val){
			i = 0;
			debounce_count = 0;
		}
	}
	close(fsr_fd);
	return result;
}

int read_in_calibration_data(){
	int fd;
	int i;
	char output_str[255];
	char *end_ptr;
	memset(output_str,0,255);


	fd = open(OUTPUT_FILE, O_RDONLY);
	if(fd == -1){
		perror("Spice_Rack_App: read_in_calibration_data - Failed to open calibration data file - ");
		syslog(LOG_DEBUG, "Spice_Rack_App: read_in_calibration_data - Failed to open calibration data filei %s", OUTPUT_FILE);
		return -1;
	}

	for(i=0;i<(SPICE_RACK_SIZE+2);i++){
		read_line(fd, output_str);
		parse_line(output_str,i);
	}

	spice_rack->empty_jar_adc = strtol(spice_rack->spices[1].spice_entries.entries[2], &end_ptr, 10);
	spice_rack->empty_rack_adc = strtol(spice_rack->spices[0].spice_entries.entries[2], &end_ptr, 10);

	close(fd);
	return 0;
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
	float mass = 0;
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
	spice_rack->empty_rack_adc = get_average_weight(read_val, read_len,10);
	syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Empty Rack Weight Reading is %s", read_val);
	printf("Empty Rack Weight Reading is %s\n", read_val);
	strcpy(spice_name, "Empty Rack");
	store_measurement(spice_num, spice_name, read_val, mass);

	printf("\nAn Empty Spice Jar is assumed to be %f Grams.\n", spice_rack->empty_jar_mass);
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
				spice_rack->empty_jar_mass = strtof(user_input_val, &end_ptr);
				if(spice_rack->empty_jar_mass == LONG_MIN || spice_rack->empty_jar_mass == LONG_MAX){
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
		spice_rack->empty_jar_adc = get_average_weight(read_val, read_len, 10);
		syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Empty Jar Weight Reading is %s", read_val);
		printf("Empty Jar Weight Reading is %s\n", read_val);
		memset(spice_name,0,MAX_SPICE_NAME_SIZE);
		snprintf(spice_name, MAX_SPICE_NAME_SIZE, "Empty Jar-%ig", (int)spice_rack->empty_jar_mass);
		store_measurement(spice_num, spice_name, read_val, mass);
		break;
	}

	printf("Please remove empty jar from Spice1 location now\n");
	while(fsr_status != 0){
		fsr_status = read_fsr_status();
	}
	spice_rack->curr_adc_reading = spice_rack->empty_rack_adc;

	printf("Now you will need to place and leave each spice on the rack. Only place one spice at a time when prompted to do so.\nGo ahead and place the first spice in Spice1 position\n");
	spice_num = 0;
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
			mass = adc_reading_to_grams();
			syslog(LOG_DEBUG,"Spice_Rack_App: calibrate_spice_rack - Weight Reading for Spice%i is %s", spice_num, read_val);
			printf("Spice%i Weight Reading is %s\n", spice_num, read_val);
			memset(spice_name,0,MAX_SPICE_NAME_SIZE);
			printf("Enter the name of this spice: ");
			if(fgets(spice_name, MAX_SPICE_NAME_SIZE, stdin)){
				spice_name[strcspn(spice_name, "\n")] = 0;
				printf("Spice Name is %s\n", spice_name);
			}
			store_measurement(spice_num, spice_name, read_val, mass);
			prev_fsr_status = fsr_status;
			if(spice_num == SPICE_RACK_SIZE){
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

int setup_spice_rack_struct(){
	int i;
	int size = 100;

	for(i=0;i<(SPICE_RACK_SIZE+2);i++){
		spice_rack->spices[i].spice_search_strings.search_strings[0] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_search_strings.search_strings[1] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_search_strings.search_strings[2] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_search_strings.search_strings[3] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_entries.entries[0] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_entries.entries[1] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_entries.entries[2] = (char *)malloc(size*sizeof(char));
		spice_rack->spices[i].spice_entries.entries[3] = (char *)malloc(size*sizeof(char));
		memset(spice_rack->spices[i].spice_search_strings.search_strings[0],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[1],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[2],0,size);
		memset(spice_rack->spices[i].spice_search_strings.search_strings[3],0,size);
		strcpy(spice_rack->spices[i].spice_search_strings.search_strings[0], "Spice_Location:\0");
		strcpy(spice_rack->spices[i].spice_search_strings.search_strings[1], "Spice_Name:\0");
		strcpy(spice_rack->spices[i].spice_search_strings.search_strings[2], "ADC_Reading:");
		strcpy(spice_rack->spices[i].spice_search_strings.search_strings[3], "Calibrated_Weight(grams):");
		memset(spice_rack->spices[i].spice_entries.entries[0],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[1],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[2],0,size);
		memset(spice_rack->spices[i].spice_entries.entries[3],0,size);
      	}

	return 0;
}

int cleanup_spice_rack_struct(){
	int i;

	for(i=0;i<(SPICE_RACK_SIZE+2);i++){
		free(spice_rack->spices[i].spice_search_strings.search_strings[0]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[1]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[2]); 
		free(spice_rack->spices[i].spice_search_strings.search_strings[3]); 
		free(spice_rack->spices[i].spice_entries.entries[0]);
		free(spice_rack->spices[i].spice_entries.entries[1]);
		free(spice_rack->spices[i].spice_entries.entries[2]);
		free(spice_rack->spices[i].spice_entries.entries[3]);
      	}

	return 0;
}

static void fsr_monitor(union sigval sigval){
	struct thread_data *td = (struct thread_data*) sigval.sival_ptr;
	if(pthread_mutex_lock(&td->lock) == 0){	
		td->fsr_prev_status = td->fsr_cur_status;
		td->fsr_cur_status = read_fsr_status();
		syslog(LOG_DEBUG, "FSR CUR=%i and FSR PREV=%i\n", td->fsr_cur_status, td->fsr_prev_status);

		if(td->fsr_cur_status != td->fsr_prev_status){
			td->fsr_alert = 1;
		}
		pthread_mutex_unlock(&td->lock);
	}
	return;
}

int setup_fsr_status_timer(struct sigevent *sigev, timer_t *timerid, struct thread_data *td, struct itimerspec *timerspec, struct timespec *start_time){
	td->hb = 0;
	td->fsr_alert = 0;
	td->fsr_cur_status = read_fsr_status();
	if(pthread_mutex_init(&td->lock,NULL) != 0){
		perror("Spice_Rack_App: setup_fsr_status_timer - Failed to initiate timer mutex - ");
		return -1;
	}
	sigev->sigev_notify = SIGEV_THREAD;
	sigev->sigev_value.sival_ptr = td;
	sigev->sigev_notify_function = fsr_monitor;
	timerspec->it_interval.tv_sec = 10;
	timerspec->it_interval.tv_nsec = 0;
	if(clock_gettime(CLOCK_MONOTONIC, start_time) != 0){
		printf("Spice_Rack_App: setup_fsr_status_timer - Error getting current time\n");
		return -1;
	}
	printf("Start time is %li\n", start_time->tv_sec);
	start_time->tv_sec = start_time->tv_sec + 2;
	if(timer_create(CLOCK_MONOTONIC, sigev, timerid) != 0){
		printf("Spice_Rack_App: setup_fsr_status_timer - Unable to create timer\n");
		return -1;
	}
	else{
		printf("Spice_Rack_App: setup_fsr_status_timer - Successfully setup timer\n");
		timerspec->it_value.tv_sec = start_time->tv_sec;
		if(timer_settime(*timerid, TIMER_ABSTIME, timerspec, NULL) != 0){
			printf("Spice_Rack_App: setup_fsr_status_timer - Unable to start timer\n");
			return -1;
		}
		printf("Spice_Rack_App: setup_fsr_status_timer - Successfully started timer\n");
	}	

	return 0;
}

int convert_fsr_stat_to_spice_num(int spice_num){
	int i;
	for(i=0; i < SPICE_RACK_SIZE; i++){
		if((spice_num >> i) == 1){
			spice_num = i+1;
			printf("Spice Num is %i\n", spice_num);
			break;
		}
	}
	return spice_num;
}

int main() {
	char *read_val;
	int read_len = 16;
//	int fsr_status;
//	int calibrate = 0;
	int fd;
	int spice_num;
	float mass = 0;
	char spice_name[32];
	struct sigevent sigev;
	struct itimerspec timerspec;
	struct timespec start_time;
	struct thread_data td;
	timer_t timerid;

	openlog(NULL,0,LOG_USER);
	syslog(LOG_DEBUG,"Spice_Rack_App: Starting Application");

	setup_calibrate_button();
	read_val = (char *)malloc(read_len * sizeof(char));
	//Must allocate spice rack size and we have flexible array members for spices and within 
	//each spice struct we have flexible arrays for search string and entries so must 
	//allocate for those too.
	printf("Got Here\n");
	spice_rack = (struct spice_rack *)malloc(sizeof(struct spice_rack) + ((SPICE_RACK_SIZE+2)*sizeof(struct spice)) + (2*(4 * sizeof(char[100]))));
	setup_spice_rack_struct();
	memset(read_val,0,read_len);
	spice_rack->curr_adc_reading = 0;
	spice_rack->empty_jar_mass = EMPTY_JAR_MASS_DEF;

	printf("Spice search string=%p and entry=%p\n", spice_rack->spices[0].spice_search_strings.search_strings[0], spice_rack->spices[0].spice_entries.entries[0]);
	
	fd = open(OUTPUT_FILE, O_RDONLY);
	if(fd == -1){
		printf("Unable to find previous calibration data to use. Performing a new calibration\n");
		calibrate_spice_rack(read_val, read_len);
	}
	else{
		printf("Found previous calibration data to use. To perform new calibration press the calibration button\n");
		read_in_calibration_data();

	}

	memset(&sigev, 0, sizeof(struct sigevent));
	memset(&timerspec, 0, sizeof(struct itimerspec));
	memset(&td, 0, sizeof(struct thread_data));

	setup_fsr_status_timer(&sigev, &timerid, &td, &timerspec, &start_time);
	while(1){
		spice_num = 0;
		if(pthread_mutex_lock(&td.lock) == 0){
			if(td.fsr_alert == 1){
				printf("FSR Status Changed!\n");
				td.fsr_alert = 0;
				if(td.fsr_cur_status > td.fsr_prev_status){
					//If a spice was added back
					spice_num = td.fsr_cur_status - td.fsr_prev_status;
					printf("Added spice back. Delta = %i\n", spice_num);
					spice_num = convert_fsr_stat_to_spice_num(spice_num);
					printf("Collecting Weight Measurement now\n");
					get_average_weight(read_val, read_len, 20);
					mass = adc_reading_to_grams();
					strncpy(spice_name, spice_rack->spices[spice_num+1].spice_entries.entries[1],32);
					printf("Spice_name in main is %s\n", spice_name);
					store_measurement(spice_num, spice_name, read_val, mass);
					//Need to update both ADC reading in output file as well as grams
				}
				else{
					//If a spice was removed
					spice_num = td.fsr_prev_status - td.fsr_cur_status;
					printf("Removed Spice. Delta = %i\n", spice_num);
					spice_num = convert_fsr_stat_to_spice_num(spice_num);
					//this collects weight and updates the prev and curr adc readings in struct
					printf("Collecting Weight Measurement now\n");
					get_average_weight(read_val, read_len, 20);
					printf("Done collecting weight \n");
				}
				
			}
			pthread_mutex_unlock(&td.lock);
		}
	}
	
	cleanup_spice_rack_struct();
	free(spice_rack);
	free(read_val);
	free_calibrate_button();
}
