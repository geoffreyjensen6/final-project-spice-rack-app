struct spice_search_strings{
	int num_search_strings;
	char *search_strings[5];
};

struct spice_entries{
	int num_entries;
	char *entries[5];
};

struct spice{
	struct spice_search_strings spice_search_strings;
	struct spice_entries spice_entries;
};

struct spice_rack{
	float empty_jar_mass;
	int empty_jar_adc;
	int empty_rack_adc;
	int previous_adc_reading;
	int curr_adc_reading;
	struct spice spices[];
};

struct thread_data{
	int hb;
	int fsr_prev_status;
	int fsr_cur_status;
	int fsr_alert;
	pthread_mutex_t lock;
};


