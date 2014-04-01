/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */

#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "trax.h"
#include "region.h"
#include "strmap.h"

// TODO: arbitrary line length (without buffering)

#define _HELPER(x) #x
#define __TOKEN_INIT TRAX_PREFIX "initialize"
#define __TOKEN_HELLO TRAX_PREFIX "hello"
#define __TOKEN_FRAME TRAX_PREFIX "frame"
#define __TOKEN_QUIT TRAX_PREFIX "quit"
#define __TOKEN_STATUS TRAX_PREFIX "status"

#define VALIDATE_HANDLE(H) assert((H->flags & TRAX_FLAG_VALID) != 0)
#define VALIDATE_SERVER_HANDLE(H) assert((H->flags & TRAX_FLAG_VALID) && (H->flags & TRAX_FLAG_SERVER))
#define VALIDATE_CLIENT_HANDLE(H) assert((H->flags & TRAX_FLAG_VALID) && !(H->flags & TRAX_FLAG_SERVER))

#define BUFFER_LENGTH 64

#define OUTPUT(H, ...) if (H->log) { fprintf(H->log, __VA_ARGS__); fflush(H->log); } fprintf(H->output, __VA_ARGS__); fflush(H->output);

#define INPUT(H, LINE) if (handle->log) { fputs(line, handle->log); fputs("\n", handle->log); fflush(handle->log); }

#define LOG(H, ...) if (H->log) { fprintf(H->log, __VA_ARGS__); fflush(H->log); }


struct trax_properties {
    StrMap *map;
};

int starts_with(const char *pre, const char *str)
{
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? 0 : strncmp(pre, str, lenpre) == 0;
}

void __output_enum(const char *key, const char *value, const void *obj) {
    
    const trax_handle* handle = (trax_handle *) obj;

    OUTPUT(handle, " %s=%s", key, value);

}

char* __read_line(FILE* file)
{

    char * line = malloc(BUFFER_LENGTH), * linep = line;
    size_t lenmax = BUFFER_LENGTH, len = lenmax;
    int c;

    if(line == NULL)
        return NULL;

    for(;;) {
        c = fgetc(file);
        if(c == EOF) {
            if (len == lenmax) {
                free(line);
                return NULL;
            }
            break;
        }

        if(--len == 0) {
            len += BUFFER_LENGTH;
            lenmax += BUFFER_LENGTH;
            char * linen = realloc(linep, lenmax);

            if(linen == NULL) {
                free(linep);
                return NULL;
            }
            line = linen + (line - linep);
            linep = linen;
        }

        if (c == '\n') break;

        *line++ = c;
    }
    *line = '\0';
    return linep;
}

char* __read_protocol_line(trax_handle* handle)
{

    char* line = NULL;

    for (;;) {
        line = __read_line(handle->input);

        if (!line) return NULL;

        INPUT(handle, line);

        if (starts_with(TRAX_PREFIX, line))
            return line;
    }
}

#define _FLAG_STRING 1

//TODO: handle \\ and \n sequences
int __next_token(const char* str, int start, char* buffer, int len, int flags) 
{
    int i;
    int s = -1;
    int e = -1;
    short quotes = 0;

    for (i = start; str[i] != '\0' ; i++) 
    {
        if (s < 0 && str[i] != ' ')
        {
            if ((flags & _FLAG_STRING != 0) && str[i] == '"') 
            {
                quotes = 1;
                s = i+1;
            }
            else s = i;
            continue;
        }
        if (s >= 0 && ((!quotes && str[i] == ' ') || (quotes && str[i] == '"' && (i == start || str[i-1] != '\''))))
        {
            e = i;
            break;
        }
    }

    buffer[0] = '\0';

    if (s < 0) return -1;

    if (e < 0 && !quotes) e = i;
    if (e < 0 && quotes) return -1;

    if (e - s > len - 1) return -1;

    memcpy(buffer, &(str[s]), e - s);

    buffer[e - s] = '\0';

    return str[e] == '\0' ? e : (quotes ? e+2 : e+1);
}

int __parse_properties(const char* line, int* pos, int size, trax_properties* properties) {

    char buffer[size];

    while ( (*pos = __next_token(line, *pos, buffer, size, _FLAG_STRING)) > 0) 
    {
        
        char* split = strstr(buffer, "=");

        if (split > 0) {
            const char* key = buffer;
            *split = '\0';
            char* value = &(split[1]);
            trax_properties_set(properties, key, value);
        }

    }

    return 1;
}

inline trax_region* __create_region(int type) {

    trax_region* reg = (trax_region*) malloc(sizeof(trax_region));

    reg->type = type;

    return reg;

}

int __parse_region(const char* line, int* pos, int size, trax_region** region) {

    int i;
    char buffer[size];

    if ( (*pos = __next_token(line, *pos, buffer, size, _FLAG_STRING)) <= 0)
        return 0;

    return parse_region(buffer, region);

}

trax_handle* trax_client_setup(FILE* input, FILE* output, FILE* log, int flags) {

    int pos = 0;
    char* line = NULL;
    int size = 0;
    char* buffer;
    trax_properties* config;

    trax_handle* client = malloc(sizeof(trax_handle));

    client->flags = (flags & ~TRAX_FLAG_SERVER) | TRAX_FLAG_VALID;

    client->log = log;
    client->input = input;
    client->output = output;

    line = __read_protocol_line(client);
    if (!line) return NULL;
    size = strlen(line) + 1;
    buffer = (char *) malloc(size * sizeof(char));

    if ((pos = __next_token(line, pos, buffer, size, _FLAG_STRING)) < 0) 
    {
        free(line);
        free(buffer);
        return NULL;
    }

    if (strcmp(buffer, __TOKEN_HELLO) == 0) 
    {

        config = trax_properties_create();

        __parse_properties(line, &pos, size, config);

        trax_properties_get_int(config, "trax.version", 1);

        char* region_type = trax_properties_get(config, "trax.region");

        client->config.format_region = TRAX_REGION_RECTANGLE;

        if (region_type && strcmp(region_type, "polygon") == 0)
            client->config.format_region = TRAX_REGION_POLYGON;
            
        free(region_type);

        // TODO: parse format info, tracker name, identifier
        
        client->config.format_image = TRAX_IMAGE_PATH;

        client->version = trax_properties_get_int(config, "trax.version", 1);

        trax_properties_release(&config);

        free(line);
        free(buffer);

        return client;
    }

    

    free(line);
    free(buffer);
    free(client);
    return NULL;

}

int trax_client_wait(trax_handle* client, trax_region** region, trax_properties* properties) {

    char* line;
    char* buffer;
    int size, pos;
    int result = TRAX_ERROR;

    (*region) = NULL;

    VALIDATE_CLIENT_HANDLE(client);

    pos = 0;
    line = __read_protocol_line(client);
    if (!line) return result;

    size = strlen(line) + 1;
    buffer = (char*) malloc(sizeof(char) * size);

    if (!line) {
        free(buffer);
        return TRAX_ERROR;

    }

    if ((pos = __next_token(line, pos, buffer, size, _FLAG_STRING)) < 0) {
        free(line);
        free(buffer);
        return TRAX_ERROR;
    }

    if (strcmp(buffer, __TOKEN_STATUS) == 0) 
    {
        result = TRAX_STATUS;

        if (!__parse_region(line, &pos, size, region)) {
            LOG(client, "WARNING: unable to parse region.\n");
            result = TRAX_ERROR; goto end;
        }  
          
        result = TRAX_STATUS;   

        if (properties) 
            __parse_properties(line, &pos, size, properties);        

end:

        free(buffer);
        free(line);
        return result;
    } else if (strcmp(buffer, __TOKEN_QUIT) == 0) {

        if (properties) 
            __parse_properties(line, &pos, size, properties);

        free(buffer);
        free(line);

        return TRAX_QUIT;
    } 

    free(buffer);
    free(line);
    return TRAX_ERROR; 

}

void trax_client_initialize(trax_handle* client, trax_image* image, trax_region* region, trax_properties* properties) {

    VALIDATE_CLIENT_HANDLE(client);

    OUTPUT(client, "%s ", __TOKEN_INIT);

    if (image->type == TRAX_IMAGE_PATH) {
        OUTPUT(client, "\"%s\" ", image->data);
    } else return;

    char* data = NULL;

    if (client->config.format_region != region->type) {

        trax_region* converted = convert_region(region, client->config.format_region);

        data = string_region(converted);

        trax_region_release(&converted);

    } else data = string_region(region);

    if (data) {
        OUTPUT(client, "\"%s\" ", data);
        free(data);
    }

    if (properties) {
        sm_enum(properties->map, __output_enum, client);
    }

    OUTPUT(client, "\n");

}

void trax_client_frame(trax_handle* client, trax_image* image, trax_properties* properties) {

    char message[1024];

    VALIDATE_CLIENT_HANDLE(client);

    assert(client->config.format_image == image->type);

    OUTPUT(client, "%s ", __TOKEN_FRAME);

    if (image->type == TRAX_IMAGE_PATH) {
        OUTPUT(client, "\"%s\" ", image->data);
    } else return;

    if (properties) {
        sm_enum(properties->map, __output_enum, client);
    }

    OUTPUT(client, "\n");

}

trax_handle* trax_server_setup(trax_configuration config, FILE* log, int flags) {

    trax_properties* properties;
    trax_handle* server = malloc(sizeof(trax_handle));

    server->flags = (flags | TRAX_FLAG_SERVER) | TRAX_FLAG_VALID;

    server->log = log;
    server->input = stdin;
    server->output = stdout;

    OUTPUT(server, __TOKEN_HELLO);

    properties = trax_properties_create();

    trax_properties_set_int(properties, "trax.version", TRAX_VERSION);

    switch (config.format_region) {
        case TRAX_REGION_RECTANGLE:
            trax_properties_set(properties, "trax.region", "rectangle");
            break;
        case TRAX_REGION_POLYGON:
            trax_properties_set(properties, "trax.region", "polygon");
            break;
        default:
            config.format_region = TRAX_REGION_RECTANGLE;
            trax_properties_set(properties, "trax.region", "rectangle");
            break;
    }

    switch (config.format_image) {
        case TRAX_IMAGE_PATH:
            trax_properties_set(properties, "trax.image", "path");
            break;
        default:
            config.format_image = TRAX_IMAGE_PATH;
            trax_properties_set(properties, "trax.image", "path");
            break;
    }

    server->config = config;
   
    sm_enum(properties->map, __output_enum, server);

    trax_properties_release(&properties);

    OUTPUT(server, "\n");

    return server;

}

int trax_server_wait(trax_handle* server, trax_image** image, trax_region** region, trax_properties* properties) 
{

    VALIDATE_SERVER_HANDLE(server);

    int pos = 0;
    char* line = __read_protocol_line(server);
    if (!line) return TRAX_ERROR;
    int size = strlen(line) + 1;
    char buffer[size];
    int result = TRAX_ERROR;

    if (!line) return TRAX_ERROR;

    if ((pos = __next_token(line, pos, buffer, size, _FLAG_STRING)) < 0) {
        free(line);

        return TRAX_ERROR;
    }

    if (strcmp(buffer, __TOKEN_FRAME) == 0) 
    {

        result = TRAX_FRAME;

        switch (server->config.format_image) {
        case TRAX_IMAGE_PATH: {
            char path[TRAX_PATH_MAX_LENGTH];
            if ((pos = __next_token(line, pos, path, TRAX_PATH_MAX_LENGTH, _FLAG_STRING)) < 0) 
                { result = TRAX_ERROR; goto end; }
            *image = trax_image_create_path(path);
            break;
        }
        default:
            result = TRAX_ERROR; goto end;
        }

        if (properties) 
            pos = __parse_properties(line, &pos, size, properties);        

        free(line);
        return result;
    } else if (strcmp(buffer, __TOKEN_QUIT) == 0) {

        if (properties) 
            pos = __parse_properties(line, &pos, size, properties);        

        free(line);
    
        return TRAX_QUIT;
    } else if (strcmp(buffer, __TOKEN_INIT) == 0) {

        result = TRAX_INITIALIZE;

        switch (server->config.format_image) {
        case TRAX_IMAGE_PATH: {
            char path[TRAX_PATH_MAX_LENGTH];
            if ((pos = __next_token(line, pos, path, TRAX_PATH_MAX_LENGTH, _FLAG_STRING)) < 0) 
                { result = TRAX_ERROR; goto end; }
            *image = trax_image_create_path(path);
            break;
        }
        default:
            result = TRAX_ERROR; goto end;
        }

        if (!__parse_region(line, &pos, size, region)) {
            result = TRAX_ERROR; goto end;
        }

        if (properties) {
            __parse_properties(line, &pos, size, properties);
        }

end:

        free(line);
        return result;

    }

    free(line);

    return TRAX_ERROR;
}

void trax_server_reply(trax_handle* server, trax_region* region, trax_properties* properties) {

    VALIDATE_SERVER_HANDLE(server);

    char* data = string_region(region);

    if (!data) return;

    OUTPUT(server, "%s ", __TOKEN_STATUS);


    if (data) {
        OUTPUT(server, "\"%s\" ", data);
        free(data);
    }

    if (properties) {
        sm_enum(properties->map, __output_enum, server);
    }

    OUTPUT(server, "\n");

}

int trax_cleanup(trax_handle** handle) {

    if (!*handle) return;

    VALIDATE_HANDLE((*handle));

// TODO: send QUIT if client

    if ((*handle)->log) {
        (*handle)->log = 0;
    }


    free(*handle);
    *handle = 0;

    return 0;
}

void trax_image_release(trax_image** image) {

    switch ((*image)->type) {
        case TRAX_IMAGE_PATH:
            free((*image)->data);
    }

    free(*image);

    *image = NULL;
}

trax_image* trax_image_create_path(const char* path) {

    trax_image* img = (trax_image*) malloc(sizeof(trax_image));

    img->type = TRAX_IMAGE_PATH;
    img->width = 0;
    img->height = 0;
    img->data = (char*) malloc(sizeof(char) * (strlen(path) + 1));
    strcpy(img->data, path);

    return img;
}

void trax_region_release(trax_region** region) {

    switch ((*region)->type) {
        case TRAX_REGION_RECTANGLE:
            break;
        case TRAX_REGION_POLYGON:
            free((*region)->data.polygon.x);
            free((*region)->data.polygon.y);
            (*region)->data.polygon.count = 0;
            break;
    }

    free(*region);

    *region = NULL;

}

trax_region* trax_region_create_rectangle(int x, int y, int width, int height) {

    trax_region* reg = __create_region(TRAX_REGION_RECTANGLE);

    reg->data.rectangle.width = width;
    reg->data.rectangle.height = height;
    reg->data.rectangle.x = x;
    reg->data.rectangle.y = y;

    return reg;

}

trax_region* trax_region_get_bounds(const trax_region* region) {

    return convert_region(region, TRAX_REGION_RECTANGLE);

}

void trax_properties_release(trax_properties** properties) {
    
    if (properties && *properties) {
        if ((*properties)->map) sm_delete((*properties)->map);
        free((*properties));
        *properties = 0;
    }

}

void trax_properties_clear(trax_properties* properties) {
    
    if (properties) {
        if (properties->map) 
            sm_delete(properties->map);
        properties->map = sm_new(32);
    }
}

trax_properties* trax_properties_create() {

    trax_properties* prop = malloc(sizeof(trax_properties));

    prop->map = sm_new(32);

    return prop;

}

void trax_properties_set(trax_properties* properties, const char* key, char* value) {

    sm_put(properties->map, key, value);

}

void trax_properties_set_int(trax_properties* properties, const char* key, int value) {

    char tmp[128];
    sprintf(tmp, "%d", value);
    trax_properties_set(properties, key, tmp);

}

void trax_properties_set_float(trax_properties* properties, const char* key, float value) {

    char tmp[128];
    sprintf(tmp, "%f", value);
    trax_properties_set(properties, key, tmp);

}

char* trax_properties_get(trax_properties* properties, const char* key) {

    int size = sm_get(properties->map, key, NULL, 0);

    if (size < 1) return NULL;

    char* value = (char *) malloc(sizeof(trax_properties) * size);

    sm_get(properties->map, key, value, size);

    return value;
}

int trax_properties_get_int(trax_properties* properties, const char* key, int def) {

    char* end;
    long ret;
    char* value = trax_properties_get(properties, key);

    if (value == NULL) return def;

    if (value[0]!='\0') {
        ret = strtol(value, &end, 10);
        ret = (*end=='\0' && end!=value) ? ret : def;
    }

    free(value);
    return (int)ret;

}


float trax_properties_get_float(trax_properties* properties, const char* key, float def) {

    char* end;
    float ret;
    char* value = trax_properties_get(properties, key);

    if (value == NULL) return def;

    if (value[0]!='\0') {
        ret = strtod(value,&end);
        ret = (*end=='\0' && end!=value) ? ret : def;
    }

    free(value);
    return ret;

}

void trax_properties_enumerate(trax_properties* properties, trax_enumerator enumerator, void* object) {
    if (properties && enumerator) {
        
        sm_enum(properties->map, enumerator, object);
    }
}

