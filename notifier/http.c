

/**
  * Include headers
  */

#include <cups/cups.h>
#include <sys/stat.h>
#include <cups/language.h>
#include <cups/string-private.h>
#include <cups/array.h>
#include <sys/select.h>
#include <cups/ipp-private.h>
#include <cups/json.h>

/**
  * Stuctures
  */

typedef struct _cups_http_s /* Http message data */
{
  int   sequence_number,    /* notify sequence number */
        printer_state,      /* printer state */
        job_id,             /* notify job id */
        job_state;          /* job state */
  time_t event_time;        /* event time */
  char  *job_name,          /* job name */
        *link_url;          /* Printer uri */
} _cups_http_t;

/**
  * Local global
  */

static char *http_password; /* Password for remote server */

/**
  * Local functions
  */

static _cups_http_t *new_msg(int sequence_number, int printer_state, int job_id,
                            int job_state, char *job_name, char *link_url,
                            time_t event_time);
static const char   *password_cb(const char *prompt);
static int          compare_msg(_cups_http_t *json_obj, _cups_http_t *json_obj2, void *data);
static int          save_json(cups_array_t *json_arr, const char *filename);
static void         add_to_json(cups_json_t *json, _cups_http_t *msg);
static void         delete_http_msg(_cups_http_t *msg);
static void         delete_json_obj(cups_json_t *json_obj);
static void         load_array(cups_array_t *arr, const char *filename);

/**
  * 'main()' Entry point for test notifier
  */

int                 /* O - Exit status */
main(int  argc,     /* I - Number of command line arguments */
     char *argv[])  /* I - Command line arguments */
{
  int           i;              /* Loop variable */
  char          scheme[32],     /* URI scheme ("http") */
                username[256],  /* Username for remote server */
                host[1024],     /* Hostname for remote server */
                resource[1024], /* JSON file */
                *options,       /* options */
                filename[1024], /* Local filename */
                newname[1024];  /* filename.N */
  int           port;           /* Port number for remote server */
  http_t        *http;          /* Connection to remote server */
  http_status_t status;         /* HTTP GET/PUT status code */
  cups_array_t  *http_msg_array; /* Http message array */
  _cups_http_t  *http_msg;      /* Http message */

  ipp_t         *event;         /* IPP event from scheduler */
  ipp_state_t   state;          /* IPP event state */
  ipp_attribute_t *printer_uptime, /* Timestamp on event */
                *printer_state, /* Printer state of event */
                *job_id,        /* Job id of event */
                *job_name,      /* Job name of event */
                *job_state,     /* Job state of event */
                *notify_sequence_number, /* Sequence number */
                *notify_printer_uri; /* Printer URI */

  char          link_url[1024], /* Link to printer */
                link_scheme[32], /* Scheme for link */
                link_username[256], /* Username for link */
                link_host[1024], /* Hostname for link */
                link_resource[1024], /* Resource for link */
                baseurl[1024];  /* Base URL */
  int           link_port,      /* Link port */
                max_events,     /* Maximum number of events to handle */
                changed,        /* Has json message data changed */
                exit_status;    /* Exit status */
  struct timeval timeout;       /* Timeout for 'select()' */
  fd_set        input;          /* Input set for 'select()' */

  /**
    * Debug prints
    */

  fprintf(stderr, "DEBUG: argc%d\n", argc);

  for (i = 0; i < argc; i++)
  {
    fprintf(stderr, "DEBUG: http notifier: argv[%d]: %s\n", i, argv[i]);
  }

  /**
    * Check if JSON feed is getting published locally or remotely
    */

  if (httpSeparateURI(HTTP_URI_CODING_ALL, argv[1], scheme, sizeof(scheme),
                      username, sizeof(username), host, sizeof(host), &port,
                      resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
  {
    fprintf(stderr, "ERROR: http notifier: Bad HTTP URI %s\n", argv[1]);
    return (1);
  }

  max_events = 20;

  if ((options = strchr(resource, '?')) != NULL)
  {
    *options++ = '\0';
    if (!strncmp(options, "max_events=", 11))
    {
      max_events = atoi(options + 11);

      if (max_events <= 0)
        max_events = 20;
    }
  }

  http_msg_array = cupsArrayNew((cups_array_func_t)compare_msg, NULL);

  if (host[0])
  {
    /**
      * Remote feed, attempt to get current file
      */

    int fd; /* Temp file */

    if ((http_password = strchr(username, ':')) != NULL)
      *http_password = '\0';

    cupsSetPasswordCB(password_cb);
    cupsSetUser(username);

    if ((fd = cupsTempFd(filename, sizeof(filename))) < 0)
    {
      fprintf(stderr, "ERROR: http notifier: Could not create temporary file %s: %s\n", filename, strerror(errno));
      return (1);
    }

    if ((http = httpConnect2(host, port, NULL, AF_UNSPEC, HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, NULL)) == NULL)
    {
      fprintf(stderr, "ERROR: http notifier: Could not connect to server %s on port %i: %s\n", host, port, strerror(errno));
      close(fd);
      unlink(filename);
      return (1);
    }

    status = cupsGetFd(http, resource, fd);

    close(fd);

    if (status != HTTP_STATUS_OK && status != HTTP_STATUS_NOT_FOUND)
    {
      fprintf(stderr, "ERROR: http notifier: Unable to GET %s from %s on port %d: %d %s\n",
              resource, host, port, status, httpStatus(status));
      httpClose(http);
      unlink(filename);
      return (1);
    }

    cupsCopyString(newname, filename, sizeof(newname));

    httpAssembleURI(HTTP_URI_CODING_ALL, baseurl, sizeof(baseurl), "http", NULL, host, port, resource);
  }
  else
  {
    const char  *cachedir,    /* CUPS_CACHEDIR */
                *server_name, /* SERVER_NAME */
                *server_port; /* SERVER_PORT */

    http = NULL;

    if ((cachedir = getenv("CUPS_CACHEDIR")) == NULL)
      cachedir = CUPS_CACHEDIR;

    if ((server_name = getenv("SERVER_NAME")) == NULL)
      server_name = "localhost";

    if ((server_port = getenv("SERVER_PORT")) == NULL)
      server_port = "631";

    snprintf(filename, sizeof(filename), "%s/http%s", cachedir, resource);
    snprintf(newname, sizeof(newname), "%s.N", resource);

    httpAssembleURIf(HTTP_URI_CODING_ALL, baseurl, sizeof(baseurl),
                     "http", NULL, server_name, atoi(server_port),
                     "/http%s", resource);
  }

  /**
    * Load the previous events from fetched file
    */

  load_array(http_msg_array, filename);

  changed = cupsArrayCount(http_msg_array) == 0;

  /**
    * Read events and update the JSON file until no events are left
    */

  for (exit_status = 0, event = NULL;;)
  {
    if (changed)
    {
      /**
        * Save messages/events to json file and upload as needed
        */
      if (save_json(http_msg_array, newname))
      {
        if (http)
        {
          /**
            * Send PUT request to update remote
            */

          if ((status = cupsPutFile(http, resource, filename)) != HTTP_STATUS_CREATED)
          {
            fprintf(stderr, "ERROR: http notifier: Unable to PUT %s from %s on port %d: %d %s\n",
                    resource, host, port, status, httpStatus(status));
          }
        }
        else
        {
          /**
            * Overwrite the existing file with new
            */

          if (rename(filename, newname))
            fprintf(stderr, "ERROR: http notifier: Unable to rename %s to %s: %s\n", filename,
                    newname, strerror(errno));
        }

        changed = 0;
      }
    }

    /**
      * Wait up to 30 seconds for event
      */
    timeout.tv_sec  = 30;
    timeout.tv_usec = 0;

    FD_ZERO(&input);
    FD_SET(0, &input);

    if (select(1, &input, NULL, NULL, &timeout) < 0)
      continue;

    if (!FD_ISSET(0, &input))
    {
      fprintf(stderr, "ERROR: http notifier: Timeout waiting for input on server side\n");
      break;
    }

    /**
      * Read next event
      */

    event = ippNew();

    while ((state = ippReadFile(0, event)) != IPP_STATE_DATA)
    {
      if (state <= IPP_STATE_IDLE)
        break;
    }

    if (state == IPP_STATE_ERROR)
      fputs("DEBUG: http notifier: ippReadFile() returned IPP_ERROR!\n", stderr);

    if (state <= IPP_STATE_IDLE)
      break;

    /**
      * Collect info from next event
      */

    job_id          = ippFindAttribute(event, "notify-job-id", IPP_TAG_INTEGER);
    job_name        = ippFindAttribute(event, "job-name", IPP_TAG_NAME);
    job_state       = ippFindAttribute(event, "job-state", IPP_TAG_ENUM);
    notify_printer_uri = ippFindAttribute(event, "notify-printer-uri",
                                          IPP_TAG_URI);
    notify_sequence_number = ippFindAttribute(event, "notify-sequence-number",
                                              IPP_TAG_INTEGER);
    printer_uptime  = ippFindAttribute(event, "printer-up-time", IPP_TAG_INTEGER);
    printer_state   = ippFindAttribute(event, "printer-state", IPP_TAG_ENUM);

    if (printer_uptime && notify_sequence_number && notify_printer_uri)
    {
      /**
        * Create new event message
        */

      if (notify_printer_uri)
      {
        httpSeparateURI(HTTP_URI_CODING_ALL, notify_printer_uri->values[0].string.text,
                        link_scheme, sizeof(link_scheme), link_username, sizeof(link_username),
                        link_host, sizeof(link_host), &link_port, link_resource, sizeof(link_resource));

        httpAssembleURI(HTTP_URI_CODING_ALL, link_url, sizeof(link_url), "http",
                        link_username, link_host, link_port, link_resource);
      }

      http_msg = new_msg(notify_sequence_number->values[0].integer,
                        printer_state->values[0].integer, job_id->values[0].integer, job_state->values[0].integer,
                        job_name->values[0].string.text, notify_printer_uri ? link_url : NULL,
                        printer_uptime->values[0].integer);

      if (!http_msg)
      {
        fprintf(stderr, "ERROR: http notifier: Unable to create json object: %s\n", strerror(errno));
        exit_status = 1;
        break;
      }

      /**
        * Add message to array
        */

      cupsArrayAdd(http_msg_array, http_msg);

      changed = 1;

      /**
        * Trimming array as needed
        */

      while (cupsArrayCount(http_msg_array) > max_events)
      {
        http_msg = cupsArrayFirst(http_msg_array);
        cupsArrayRemove(http_msg_array, http_msg);
        delete_http_msg(http_msg);
      }
    }

    if (job_name)
      free(job_name);

    ippDelete(event);
    event = NULL;
  }

  /**
    * This point is reached when idle or an error occurs
    */

  ippDelete(event);

  if (http)
  {
    unlink(filename);
    httpClose(http);
  }

  return (exit_status);
}

/**
  * 'compare_msg()' - Compare two messages
  */
static int                    /* O - Result of comparison */
compare_msg(_cups_http_t *a,  /* I - First message */
            _cups_http_t *b,  /* I - Second message */
            void *data)       /* Unused */
{
  (void)data;
  return (a->sequence_number - b->sequence_number);
}

/**
  * 'delete_http_message()' - Free all memory used by a message
  */
static void
delete_http_msg(_cups_http_t *msg) /* I - http message to free */
{
  if (msg->job_name)
    free(msg->job_name);

  if (msg->link_url)
    free(msg->link_url);

  free(msg);
}

/**
  * 'new_msg()' - Create new http message
  */
static _cups_http_t             /* O - New message */
*new_msg(int  sequence_number,  /* I - notify-sequence-number */
         int  printer_state,    /* I - printer-state */
         int  job_id,           /* I - job-id */
         int  job_state,        /* I - job-state */
         char *job_name,        /* I - job-name */
         char *link_url,        /* I - link to printer */
         time_t event_time)     /* I - Data/time of event */
{
  _cups_http_t *msg;            /* New message */

  if ((msg = calloc(1, sizeof(_cups_http_t))) == NULL)
  {
#ifdef __clang_analyzer__
    /**
      * Free calls to make Clang happy
      */
    free(link_url);
    free(job_name);
#endif // __clang_analyzer__
    return (NULL);
  }

  msg->sequence_number  = sequence_number;
  msg->printer_state    = printer_state;
  msg->job_id           = job_id;
  msg->job_state        = job_state;
  msg->job_name         = job_name;
  msg->link_url         = link_url;
  msg->event_time       = event_time;

  return (msg);
}
 
/**
  * 'password_cb()' - Return cached password
  */
static const char                 /* O - Cached password */
*password_cb(const char *prompt)  /* I - Prompt string, unused */
{
  (void)prompt;
  return (http_password);
}
 
/**
  * 'load_array()' - Load an existing JSON feed file
  */
static void
load_array(cups_array_t *arr,       /* I - http message array */
           const char   *filename)  /* I - File to load */
{
  cups_json_t   *json_obj,          /* Root json object from file */
                *json_events_arr,   /* JSON array node that holds the events */
                *current,           /* Current child JSON object in events array */
                *link_url,          /* JSON node link-url */
                *job_name,          /* JSON node job-name */
                *event_time,        /* JSON node event-time */
                *sequence_number,   /* JSON node sequence-number */
                *printer_state,     /* JSON node printer-state */
                *job_state,         /* JSON node job-state */
                *job_id;            /* JSON node job-id */
  _cups_http_t  *http_msg;          /* New http message */

  /**
    * Import file and read it into object
    */
  if ((json_obj = cupsJSONImportFile(filename)) == NULL)
  {
    fprintf(stderr, "ERROR: http notifier: Unable to load JSON file %s: %s\n",
            filename, strerror(errno));
    return;
  }

  link_url        = NULL;
  job_name        = NULL;
  job_state       = NULL;
  job_id          = NULL;
  printer_state   = NULL;
  sequence_number = NULL;
  event_time      = NULL;

  /**
    * Check for events array node and exit if not found
    */

  if ((json_events_arr = cupsJSONFind(json_obj, "events")) == NULL)
  {
    fprintf(stderr, "ERROR: http notifier: Unable to find events in %s", filename);
    return;
  }
  else
  {
    for (current = cupsJSONGetChild(json_events_arr, sizeof(CUPS_JTYPE_ARRAY));
         current;
         current = cupsJSONGetSibling(current))
    {
      event_time      = cupsJSONFind(current, "event-time");
      job_id          = cupsJSONFind(current, "job-id");
      job_name        = cupsJSONFind(current, "job-name");
      job_state       = cupsJSONFind(current, "job-state");
      link_url        = cupsJSONFind(current, "link-url");
      printer_state   = cupsJSONFind(current, "printer-state");
      sequence_number = cupsJSONFind(current, "sequence-number");

      if (link_url && job_name && sequence_number)
      {
        http_msg = new_msg((int)cupsJSONGetNumber(sequence_number),
                            (int)cupsJSONGetNumber(printer_state),
                            (int)cupsJSONGetNumber(job_id),
                            (int)cupsJSONGetNumber(job_state),
                            (char *)cupsJSONGetString(job_name),
                            (char *)cupsJSONGetString(link_url),
                            (time_t)cupsJSONGetNumber(event_time));

        if (http_msg)
        {
          cupsArrayAdd(arr, http_msg);
        }
      }
    }
  }

  /**
    * Free all the memory
    */

  if (http_msg)
    delete_http_msg(http_msg);

  if (link_url)
    delete_json_obj(link_url);

  if (job_name)
    delete_json_obj(job_name);

  if (job_id)
    delete_json_obj(job_id);
  
  if (job_state)
    delete_json_obj(job_state);

  if (printer_state)
    delete_json_obj(printer_state);

  if (sequence_number)
    delete_json_obj(sequence_number);

  if (event_time)
    delete_json_obj(event_time);

  if (json_events_arr)
    delete_json_obj(json_events_arr);

  if (json_obj)
    delete_json_obj(json_obj);

  if (current)
    delete_json_obj(current);
}
 
/**
  * 'save_json()' - Save messages to JSON file
  */
static int                        /* O - 1 on success, 0 on failure */
save_json(cups_array_t *http_arr, /* I - http message array */
          const char   *filename) /* I - File to save to */
{
  cups_json_t *json_root,         /* JSON root object */
              *json_events,       /* JSON array node key 'events' */
              *event_obj;         /* JSON node, array child object */
  _cups_http_t *current;          /* current message in http message array */
  int         is_exported;        /* Success or failure return value */
 
  json_root    = cupsJSONNew(NULL, NULL, CUPS_JTYPE_OBJECT);
  json_events  = cupsJSONNew(json_root, cupsJSONNewKey(json_root, NULL, "events"), CUPS_JTYPE_ARRAY);

  for (current = (_cups_http_t *)cupsArrayLast(http_arr);
       current;
       current = (_cups_http_t *)cupsArrayPrev(http_arr))
  {
    if ((event_obj = cupsJSONNew(json_events, NULL, CUPS_JTYPE_OBJECT)) == NULL)
    {
      fprintf(stderr, "ERROR: http notifier: invalid parent type\n");
      return (1);
    }
    add_to_json(event_obj, current);
  }
 
  is_exported = cupsJSONExportFile(json_root, filename);

  if (current)
    delete_http_msg(current);

  if (json_root)
  {
    fprintf(stderr, "DEBUG: http notifier: save_json() deleting json_root obj\n");
    delete_json_obj(json_root);
  }

  return is_exported;
}
 
/**
  * 'delete_json_obj()' - Clean up json node
  */
static void
delete_json_obj(cups_json_t *json) /* I - JSON node to delete*/
{
  cupsJSONDelete(json);
}

/**
  * 'add_to_json()' - Build event object with http message data
  */
static void
add_to_json(cups_json_t *json,      /* I - JSON object node */
            _cups_http_t *message)  /* http message data */
{
  cupsJSONNewNumber(json, cupsJSONNewKey(json, NULL, "sequence-number"),
                    message->sequence_number);
  cupsJSONNewNumber(json, cupsJSONNewKey(json, NULL, "printer-state"),
                    message->printer_state ? message->printer_state : -1);
  cupsJSONNewNumber(json, cupsJSONNewKey(json, NULL, "job-state"),
                    message->job_state ? message->job_state : -1);
  cupsJSONNewNumber(json, cupsJSONNewKey(json, NULL, "job-id"),
                    message->job_id ? message->job_id : -1);
  cupsJSONNewNumber(json, cupsJSONNewKey(json, NULL, "event-time"),
                    message->event_time);
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "job-name"),
                    message->job_name ? message->job_name : "");
  cupsJSONNewString(json, cupsJSONNewKey(json, NULL, "link-url"),
                    message->link_url);
}
