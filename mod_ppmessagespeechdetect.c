/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2013, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Guijin Ding <dingguijin@gmail.com>
 *
 * mod_ppmessage - PPMessage
 *
 *
 */

#include <switch.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_ppmessage_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ppmessage_shutdown);
SWITCH_MODULE_DEFINITION(mod_ppmessage, mod_ppmessage_load, mod_ppmessage_shutdown, NULL);


#define MAX_SEGMENTS 1024

static switch_mutex_t *MUTEX = NULL;
static switch_event_node_t *NODE = NULL;

typedef enum {
	AVD_NULL = (1 << 0),
	AVD_VOICE = (1 << 1),
    AVD_SILENCE = (1 << 2)
} avd_status_t;

typedef struct {
	char* speech;
    int length;
	int index;
} avd_segment_t;

static struct {
	char *model8k;
	char *model16k;
	char *dictionary;
	char *language_weight;
	uint32_t thresh;
	int no_input_timeout;
	int speech_timeout;
	switch_bool_t start_input_timers;
	int confidence_threshold;
	uint32_t silence_hits;
	uint32_t listen_hits;
	int auto_reload;
	switch_memory_pool_t *pool;
} globals;

typedef enum {
	PSFLAG_HAS_TEXT = (1 << 0),
	PSFLAG_READY = (1 << 1),
	PSFLAG_BARGE = (1 << 2),
	PSFLAG_ALLOCATED = (1 << 3),
	PSFLAG_INPUT_TIMERS = (1 << 4),
	PSFLAG_START_OF_SPEECH = (1 << 5),
	PSFLAG_NOINPUT_TIMEOUT = (1 << 6),
	PSFLAG_SPEECH_TIMEOUT = (1 << 7),
	PSFLAG_NOINPUT = (1 << 8),
	PSFLAG_NOMATCH = (1 << 9)
} psflag_t;

typedef struct {
	avd_status_t pre_status;
	avd_status_t avd_status;
	avd_segment_t **segments;
	int segment_index;
	
	uint32_t flags;
	switch_mutex_t *flag_mutex;
	uint32_t org_voice_hits;
	uint32_t org_silence_hits;
	uint32_t thresh;
	uint32_t voice_hits;
	uint32_t silence_hits;
	uint32_t listen_hits;
	uint32_t listening;
	uint32_t countdown;
	int no_input_timeout;
	int speech_timeout;
	switch_bool_t start_input_timers;
	switch_time_t silence_time;
	int confidence_threshold;
	char *hyp;
	char *grammar;
	int32_t score;
	int32_t confidence;
	char const *uttid;
} ppmessage_t;


static double get_avg_energy_score(int16_t* data, unsigned int samples)
{
	uint32_t score, j = 0;
	double energy = 0;

	/* Do simple energy threshold for VAD */
	for (j = 0; j < samples; j++) {
		energy += abs(data[j]);
	}
	
	score = (uint32_t) (energy / samples);

	//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "score %u\n", score);	
	return score;
}

static void init_segments(ppmessage_t* ps)
{
	ps->segment_index = 0;
	memset(ps->segments, 0, sizeof(avd_segment_t*) * MAX_SEGMENTS);
	return;
}

static void push_segment(ppmessage_t* ps, void* data, unsigned int len)
{
	avd_segment_t* segment = NULL;
	void* speech = NULL;

	speech = malloc(len); 
	segment = (avd_segment_t*)malloc(sizeof(avd_segment_t));
	
	memset(segment, 0, sizeof(avd_segment_t));
	memcpy(speech, data, len);
	
	segment->index = ps->segment_index;
	segment->speech = speech;
	segment->length = len;

	ps->segments[ps->segment_index] = segment;	
	ps->segment_index++;
	return;
}

static char* concat_segments(ppmessage_t* ps)
{
	int i, len, offset, need_bytes;
	char *dst, *encoded;

	if (!ps->segment_index) {
		return NULL;
	}

	len = 0;
	for (i = 0; i < ps->segment_index; i++) {
		len += ps->segments[i]->length;
	}

	dst = (char*)malloc(len);
	offset = 0;
	for (i = 0; i < ps->segment_index; i++) {
		memcpy(&dst[offset], ps->segments[i]->speech, ps->segments[i]->length);
		offset += ps->segments[i]->length;
		free(ps->segments[i]->speech);
		free(ps->segments[i]);
		ps->segments[i] = NULL;
	}

	need_bytes = 3 * len + 1;
	encoded = (char*)malloc(need_bytes);
	memset(encoded, 0, need_bytes);
	switch_b64_encode((unsigned char*)dst, len, (unsigned char*)encoded, (unsigned int)need_bytes);
	free(dst);
	
	return encoded;
}

static void avd_status_null(ppmessage_t *ps, int16_t *data, unsigned int samples)
{
	double score = get_avg_energy_score(data, samples);

	ps->voice_hits = 0;
	ps->silence_hits = 0;
	
	if (score >= ps->thresh) {
		init_segments(ps);
		ps->avd_status = AVD_VOICE;
		return;
	}

	ps->avd_status = AVD_SILENCE;
	return;
}

static void avd_status_silence(ppmessage_t *ps, int16_t *data, unsigned int samples)
{
	double score = get_avg_energy_score(data, samples);

	if (score >= ps->thresh) {
		ps->voice_hits = ps->voice_hits + 1;
		if (ps->voice_hits >= ps->org_voice_hits) {
			init_segments(ps);
			ps->avd_status = AVD_VOICE;
		}
		return;
	}

	ps->voice_hits = 0;
	return;
}

static void avd_status_voice(ppmessage_t *ps, int16_t *data, unsigned int samples)
{
	double score = get_avg_energy_score(data, samples);

	if (score < ps->thresh) {
		ps->silence_hits = ps->silence_hits + 1;
		if (ps->silence_hits >= ps->org_silence_hits) {
			ps->avd_status = AVD_SILENCE;
		}
		return;
	}

	if (ps->segment_index + 1 == MAX_SEGMENTS) {
		ps->avd_status = AVD_SILENCE;
		return;
	}

	ps->silence_hits = 0;
	return;

}

static void transfer_avd_status(ppmessage_t *ps, int16_t *data, unsigned int samples)
{
	ps->pre_status = ps->avd_status;

	if (ps->avd_status == AVD_NULL) {
		avd_status_null(ps, data, samples);
		return;
	}

	if (ps->avd_status == AVD_SILENCE) {
		avd_status_silence(ps, data, samples);
		return;
	}

	if (ps->avd_status == AVD_VOICE) {
		avd_status_voice(ps, data, samples);
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error avd status %d\n", ps->avd_status);	
	return;
}

/*! function to open the asr interface */
static switch_status_t ppmessage_asr_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest, switch_asr_flag_t *flags)
{

	ppmessage_t *ps;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_open ....\n");

	if (!(ps = (ppmessage_t *) switch_core_alloc(ah->memory_pool, sizeof(*ps)))) {
		return SWITCH_STATUS_MEMERR;
	}

	ps->avd_status = AVD_NULL;
	ps->pre_status = AVD_NULL;
	ps->segments = (avd_segment_t**)malloc(sizeof(avd_segment_t*) * MAX_SEGMENTS);
	init_segments(ps);
	
	switch_mutex_init(&ps->flag_mutex, SWITCH_MUTEX_NESTED, ah->memory_pool);
	ah->private_info = ps;

	if (rate == 8000) {
		ah->rate = 8000;
	} else if (rate == 16000) {
		ah->rate = 16000;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid rate %d. Only 8000 and 16000 are supported.\n", rate);
	}

	codec = "L16";

	ah->codec = switch_core_strdup(ah->memory_pool, codec);

	globals.thresh = 300;
	globals.silence_hits = 10;
	
	ps->thresh = globals.thresh;
	ps->org_silence_hits = globals.silence_hits;
	ps->silence_hits = 0;
	ps->voice_hits = 0;
	ps->org_voice_hits = 0;
	
	ps->listen_hits = globals.listen_hits;
	ps->start_input_timers = globals.start_input_timers;
	ps->no_input_timeout = globals.no_input_timeout;
	ps->speech_timeout = globals.speech_timeout;
	ps->confidence_threshold = globals.confidence_threshold;

	return SWITCH_STATUS_SUCCESS;
}

/*! function to load a grammar to the asr interface */
static switch_status_t ppmessage_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_load_grammar ....\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! function to unload a grammar to the asr interface */
static switch_status_t ppmessage_asr_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_unload_grammar ....\n");

	return SWITCH_STATUS_SUCCESS;
}

/*! function to close the asr interface */
static switch_status_t ppmessage_asr_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_close ....\n");
	ppmessage_t *ps = (ppmessage_t *) ah->private_info;
	switch_safe_free(ps->segments);
	return SWITCH_STATUS_SUCCESS;
}

/*! function to feed audio to the ASR */
static switch_status_t ppmessage_asr_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_feed: %d\n", len);
	ppmessage_t *ps = (ppmessage_t *) ah->private_info;	
	transfer_avd_status(ps, (int16_t *) data, len / 2);
	
	if (ps->avd_status == AVD_VOICE) {
		push_segment(ps, data, len);
	}
	return SWITCH_STATUS_SUCCESS;
}

/*! function to pause recognizer */
static switch_status_t ppmessage_asr_pause(switch_asr_handle_t *ah)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_pause ....\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! function to resume recognizer */
static switch_status_t ppmessage_asr_resume(switch_asr_handle_t *ah)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_resume ....\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! function to read results from the ASR*/
static switch_status_t ppmessage_asr_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	ppmessage_t *ps = (ppmessage_t *) ah->private_info;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "asr_check_results.\n");

	/* if (ps->avd_status == AVD_VOICE) { */
	/* 	return SWITCH_STATUS_SUCCESS; */
	/* } */
	
	if (ps->avd_status == AVD_SILENCE && ps->pre_status == AVD_VOICE) {
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

/*! function to read results from the ASR */
static switch_status_t ppmessage_asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags)
{
	ppmessage_t *ps = (ppmessage_t *) ah->private_info;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "asr_get_results.\n");
	if (ps->avd_status == AVD_VOICE) {
		return SWITCH_STATUS_BREAK;
	}
	
	if (ps->pre_status == AVD_VOICE && ps->avd_status == AVD_SILENCE) {
		char* segments = concat_segments(ps);
		if (segments == NULL) {
			return SWITCH_STATUS_BREAK;
		}
		*xmlstr = segments;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%lu.\n", strlen(*xmlstr));
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_BREAK;
}

/*! function to start input timeouts */
static switch_status_t ppmessage_asr_start_input_timers(switch_asr_handle_t *ah)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ppmessage_asr_start_input_timers ....\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! set text parameter */
static void ppmessage_asr_text_param(switch_asr_handle_t *ah, char *param, const char *val)
{
	return;
}

/*! set numeric parameter */
static void ppmessage_asr_numeric_param(switch_asr_handle_t *ah, char *param, int val)
{
	return;
}

/*! set float parameter */
static void ppmessage_asr_float_param(switch_asr_handle_t *ah, char *param, double val)
{
	return;
}

static switch_status_t load_config(void)
{
	return SWITCH_STATUS_SUCCESS;
}

static void do_load(void)
{
	switch_mutex_lock(MUTEX);
	load_config();
	switch_mutex_unlock(MUTEX);
}

static void event_handler(switch_event_t *event)
{
	if (globals.auto_reload) {
		do_load();
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "PPMessage Reloaded\n");
	}
}

SWITCH_MODULE_LOAD_FUNCTION(mod_ppmessage_load)
{
	switch_asr_interface_t *asr_interface;

	switch_mutex_init(&MUTEX, SWITCH_MUTEX_NESTED, pool);

	globals.pool = pool;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_ppmessage_load!\n");
	if ((switch_event_bind_removable(modname, SWITCH_EVENT_RELOADXML, NULL, event_handler, NULL, &NODE) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
	}

	do_load();

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	asr_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ASR_INTERFACE);
	asr_interface->interface_name = "ppmessage";
	asr_interface->asr_open = ppmessage_asr_open;
	asr_interface->asr_load_grammar = ppmessage_asr_load_grammar;
	asr_interface->asr_unload_grammar = ppmessage_asr_unload_grammar;
	asr_interface->asr_close = ppmessage_asr_close;
	asr_interface->asr_feed = ppmessage_asr_feed;
	asr_interface->asr_resume = ppmessage_asr_resume;
	asr_interface->asr_pause = ppmessage_asr_pause;
	asr_interface->asr_check_results = ppmessage_asr_check_results;
	asr_interface->asr_get_results = ppmessage_asr_get_results;
	asr_interface->asr_start_input_timers = ppmessage_asr_start_input_timers;
	asr_interface->asr_text_param = ppmessage_asr_text_param;
	asr_interface->asr_numeric_param = ppmessage_asr_numeric_param;
	asr_interface->asr_float_param = ppmessage_asr_float_param;

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ppmessage_shutdown)
{
	switch_event_unbind(&NODE);
	return SWITCH_STATUS_UNLOAD;
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
