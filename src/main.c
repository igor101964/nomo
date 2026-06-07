/*
 * nomo - ASCII Art Generator, GTK4
 * Models from ~/.mshellrc, prompts from NOMO_DIR/sysprompts/
 * Modes: Model (LLM via mshell IPC) and Figlet (local font render)
 */

#define _GNU_SOURCE
#define GDK_DISABLE_DEPRECATION_WARNINGS
#define GTK_DISABLE_DEPRECATION_WARNINGS
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

/* paths */
#define NOMO_DIR      "/home/igor/nomo"
#define INPUT_FILE    NOMO_DIR "/input.txt"
#define RESULT_FILE   NOMO_DIR "/ascii_result.txt"
#define DONE_FLAG     NOMO_DIR "/done.flag"
#define WORKFLOW      NOMO_DIR "/generate.md"
#define MSHELLRC      "/.mshellrc"
#define SYSPROMPT_DIR NOMO_DIR "/sysprompts"
#define FIGLET_DIR    "/usr/share/figlet"
#define FIGLET_EXT    ".tlf"

#define MODEL_NAME_MAX 192
#define VENDOR_MAX      64
#define SYSPROMPT_MAX  8192
#define MAX_PROMPTS      64
#define MAX_FONTS        64
#define PATH_MAX_LEN    768

static const char BUILTIN_SYSPROMPT[] =
    "You are a world-class ASCII art generator.\n"
    "Rules you MUST follow:\n"
    "1. Output ONLY the ASCII art. Zero words, zero explanations,\n"
    "   zero markdown, zero code fences.\n"
    "2. Use a rich variety of characters:\n"
    "   @ # % & * + = - _ . , : ; ! | / \\ ( ) [ ] { } < > ~ ^ `\n"
    "3. Width: 60-70 characters. Height: 20-30 lines.\n"
    "4. NEVER output text before or after the art.\n"
    "   The very first character must be part of the art.";

typedef struct { const char *name; const char *css_color; } ColorEntry;
static const ColorEntry COLORS[] = {
    { "Default", NULL      },
    { "Green",   "#33cc55" },
    { "Amber",   "#ffb300" },
    { "Cyan",    "#00bcd4" },
    { "White",   "#f0f0f0" },
    { "Red",     "#ff5252" },
    { "Magenta", "#e040fb" },
    { "Blue",    "#82b1ff" },
    { "Orange",  "#ff6d00" },
};
#define N_COLORS ((int)(sizeof(COLORS)/sizeof(COLORS[0])))

/* models */
static char g_model_name[3][MODEL_NAME_MAX];
static char g_model_short[3][48];

static int extract_value(const char *line, const char *key, char *out, int outlen) {
    while (*line==' '||*line=='\t') line++;
    if (*line=='#') return 0;
    if (strncmp(line,"export ",7)==0) line+=7;
    while (*line==' '||*line=='\t') line++;
    size_t klen=strlen(key);
    if (strncmp(line,key,klen)!=0||line[klen]!='=') return 0;
    const char *p=line+klen+1;
    char quote=0;
    if (*p=='\''||*p=='"') quote=*p++;
    int i=0;
    while (*p&&i<outlen-1) {
        if (quote&&*p==quote) break;
        if (!quote&&(*p=='\n'||*p=='\r'||*p=='#')) break;
        out[i++]=*p++;
    }
    out[i]='\0';
    while (i>0&&(out[i-1]==' '||out[i-1]=='\t')) out[--i]='\0';
    return i>0;
}

static void load_model_names(void) {
    for (int s=0;s<3;s++) {
        snprintf(g_model_name[s],  MODEL_NAME_MAX, "Model %d", s+1);
        snprintf(g_model_short[s], sizeof(g_model_short[0]), "%d: Model %d", s+1, s+1);
    }
    const char *home=getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path,sizeof(path),"%s%s",home,MSHELLRC);
    FILE *f=fopen(path,"r");
    if (!f) return;
    char slot_model[3][MODEL_NAME_MAX];
    char slot_vendor[3][VENDOR_MAX];
    int  slot_active[3]={0,0,0};
    memset(slot_model, 0,sizeof(slot_model));
    memset(slot_vendor,0,sizeof(slot_vendor));
    char line[1024];
    while (fgets(line,(int)sizeof(line),f)) {
        char val[MODEL_NAME_MAX];
        for (int s=1;s<=3;s++) {
            char key[40];
            snprintf(key,sizeof(key),"OLLAMA%d_MODEL",s);
            if (extract_value(line,key,val,(int)sizeof(val))) {
                strncpy(slot_model[s-1],val,MODEL_NAME_MAX-1);
                slot_active[s-1]=1;
            }
            snprintf(key,sizeof(key),"OLLAMA%d_VENDOR",s);
            if (extract_value(line,key,val,VENDOR_MAX))
                strncpy(slot_vendor[s-1],val,VENDOR_MAX-1);
        }
    }
    fclose(f);
    for (int s=0;s<3;s++) {
        if (!slot_active[s]) continue;
        if (slot_vendor[s][0])
            snprintf(g_model_name[s],MODEL_NAME_MAX,"%.60s / %.120s",
                     slot_vendor[s],slot_model[s]);
        else
            snprintf(g_model_name[s],MODEL_NAME_MAX,"%.185s",slot_model[s]);
        const char *slash=strrchr(slot_model[s],'/');
        const char *sm=slash?slash+1:slot_model[s];
        snprintf(g_model_short[s],sizeof(g_model_short[0]),"%d: %.40s",s+1,sm);
    }
}

/* figlet fonts */
static char g_fonts[MAX_FONTS][64];
static int  g_n_fonts=0;

static void scan_figlet_fonts(void) {
    g_n_fonts=0;
    DIR *d=opendir(FIGLET_DIR);
    if (!d) return;
    struct dirent *e;
    size_t extlen=strlen(FIGLET_EXT);
    while ((e=readdir(d))&&g_n_fonts<MAX_FONTS) {
        const char *nm=e->d_name;
        size_t nlen=strlen(nm);
        if (nlen<=extlen) continue;
        if (strcmp(nm+nlen-extlen,FIGLET_EXT)!=0) continue;
        int clen=(int)nlen-(int)extlen;
        if (clen>63) clen=63;
        strncpy(g_fonts[g_n_fonts],nm,(size_t)clen);
        g_fonts[g_n_fonts][clen]='\0';
        g_n_fonts++;
    }
    closedir(d);
    for (int i=0;i<g_n_fonts-1;i++)
        for (int j=i+1;j<g_n_fonts;j++)
            if (strcmp(g_fonts[i],g_fonts[j])>0) {
                char tmp[64];
                strncpy(tmp,g_fonts[i],63); tmp[63]='\0';
                strncpy(g_fonts[i],g_fonts[j],63);
                strncpy(g_fonts[j],tmp,63);
            }
}

static char *run_figlet(const char *text, const char *font) {
    char cmd[1024];
    if (font&&*font)
        snprintf(cmd,sizeof(cmd),
            "figlet -d '%s' -f '%s' -- '%s' 2>/dev/null",
            FIGLET_DIR, font, text);
    else
        snprintf(cmd,sizeof(cmd),"figlet -- '%s' 2>/dev/null",text);
    FILE *p=popen(cmd,"r");
    if (!p) return NULL;
    size_t sz=8192,pos=0;
    char *buf=malloc(sz);
    if (!buf){pclose(p);return NULL;}
    int c;
    while ((c=fgetc(p))!=EOF) {
        if (pos+2>=sz){sz*=2;char *nb=realloc(buf,sz);if(!nb){free(buf);pclose(p);return NULL;}buf=nb;}
        buf[pos++]=(char)c;
    }
    buf[pos]='\0';
    pclose(p);
    return buf;
}

/* sysprompts */
typedef struct { char name[128]; char path[PATH_MAX_LEN]; } PromptFile;
static PromptFile g_prompts[MAX_PROMPTS];
static int        g_n_prompts=0;
static int        g_prompt_idx=0;
static char       g_sysprompt_dir[PATH_MAX_LEN];
static char       g_current_sysprompt[SYSPROMPT_MAX];

static void ensure_sysprompt_dir(void) {
    snprintf(g_sysprompt_dir,sizeof(g_sysprompt_dir),"%s",SYSPROMPT_DIR);
    mkdir(g_sysprompt_dir,0755);
}

static void load_sysprompt(int idx) {
    if (idx<=0||idx>g_n_prompts) {
        strncpy(g_current_sysprompt,BUILTIN_SYSPROMPT,SYSPROMPT_MAX-1);
        g_current_sysprompt[SYSPROMPT_MAX-1]='\0';
        return;
    }
    FILE *f=fopen(g_prompts[idx-1].path,"r");
    if (!f){strncpy(g_current_sysprompt,BUILTIN_SYSPROMPT,SYSPROMPT_MAX-1);return;}
    int n=(int)fread(g_current_sysprompt,1,SYSPROMPT_MAX-1,f);
    fclose(f);
    g_current_sysprompt[n<0?0:n]='\0';
}

static void scan_sysprompts(void) {
    g_n_prompts=0;
    DIR *d=opendir(g_sysprompt_dir);
    if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))&&g_n_prompts<MAX_PROMPTS) {
        const char *nm=e->d_name;
        size_t nlen=strlen(nm);
        if (nlen<5) continue;
        if (strcmp(nm+nlen-4,".prm")!=0) continue;
        snprintf(g_prompts[g_n_prompts].path,PATH_MAX_LEN,
                 "%.400s/%.255s",g_sysprompt_dir,nm);
        int clen=(int)nlen-4;
        if (clen>127) clen=127;
        strncpy(g_prompts[g_n_prompts].name,nm,(size_t)clen);
        g_prompts[g_n_prompts].name[clen]='\0';
        g_n_prompts++;
    }
    closedir(d);
    for (int i=0;i<g_n_prompts-1;i++)
        for (int j=i+1;j<g_n_prompts;j++)
            if (strcmp(g_prompts[i].name,g_prompts[j].name)>0) {
                PromptFile tmp=g_prompts[i];
                g_prompts[i]=g_prompts[j];
                g_prompts[j]=tmp;
            }
}

/* IPC */
static int g_ipc_fd_write=-1;
static int g_ipc_fd_read =-1;
static int g_ipc_seq     =1;
static int g_current_slot=1;

static void ipc_connect(void) {
    if (g_ipc_fd_write>=0) return;
    const char *ps=getenv("MSHELL_IPC_PID");
    if (!ps) return;
    int pid=atoi(ps);
    char tx[64],rx[64];
    snprintf(tx,sizeof(tx),"/tmp/mide_to_msh_%d",pid);
    snprintf(rx,sizeof(rx),"/tmp/msh_to_mide_%d",pid);
    g_ipc_fd_write=open(tx,O_WRONLY|O_NONBLOCK);
    g_ipc_fd_read =open(rx,O_RDONLY|O_NONBLOCK);
}
static void ipc_send_raw(const char *json) {
    ipc_connect();
    if (g_ipc_fd_write<0) return;
    (void)write(g_ipc_fd_write,json,strlen(json));
    (void)write(g_ipc_fd_write,"\n",1);
}
static void ipc_set_provider(int slot) {
    char json[128];
    snprintf(json,sizeof(json),
        "{\"v\":1,\"id\":\"%d\",\"cmd\":31,\"args\":\"%d\"}",g_ipc_seq++,slot);
    ipc_send_raw(json);
}
static char *json_escape(const char *s) {
    size_t len=strlen(s);
    char *out=malloc(len*4+4);
    if (!out) return NULL;
    size_t oi=0;
    for (size_t i=0;i<len;i++) {
        if      (s[i]=='"')  {out[oi++]='\\';out[oi++]='"';}
        else if (s[i]=='\\') {out[oi++]='\\';out[oi++]='\\';}
        else if (s[i]=='\n') {out[oi++]='\\';out[oi++]='n';}
        else if (s[i]=='\r') {}
        else if (s[i]=='\t') {out[oi++]='\\';out[oi++]='t';}
        else                 {out[oi++]=s[i];}
    }
    out[oi]='\0';
    return out;
}
static void ipc_send_sysprompt(void) {
    char *ep=json_escape(g_current_sysprompt);
    if (!ep) return;
    size_t jlen=strlen(ep)+64;
    char *json=malloc(jlen);
    if (json) {
        snprintf(json,jlen,
            "{\"v\":1,\"id\":\"%d\",\"cmd\":1,\"text\":\"%s\",\"model\":%d}",
            g_ipc_seq++,ep,g_current_slot);
        ipc_send_raw(json);
        free(json);
    }
    free(ep);
}
static void ipc_send_prompt(const char *prompt,int slot) {
    ipc_connect();
    if (g_ipc_fd_write<0) return;
    char *ep=json_escape(prompt);
    if (!ep) return;
    size_t jlen=strlen(ep)+64;
    char *json=malloc(jlen);
    if (json) {
        snprintf(json,jlen,
            "{\"v\":1,\"id\":\"%d\",\"cmd\":1,\"code\":\"%s\",\"model\":%d}",
            g_ipc_seq++,ep,slot);
        ipc_send_raw(json);
        free(json);
    }
    free(ep);
}
static int json_unescape(const char *src,char *dst,int maxlen) {
    int di=0;
    while (*src&&di<maxlen-1) {
        if (*src=='\\'&&*(src+1)) {
            src++;
            if      (*src=='n')  dst[di++]='\n';
            else if (*src=='t')  dst[di++]='\t';
            else if (*src=='"')  dst[di++]='"';
            else if (*src=='\\') dst[di++]='\\';
            else                 dst[di++]=*src;
        } else if (*src=='"') {break;}
        else {dst[di++]=*src;}
        src++;
    }
    dst[di]='\0';
    return di;
}

/* state */
typedef enum {ST_IDLE,ST_WAITING,ST_DONE,ST_ERROR} State;
static State    g_state     =ST_IDLE;
static char     g_accum[65536];
static int      g_accum_len =0;
static guint    g_poll_timer=0;
static gboolean g_dark_mode =TRUE;
static int      g_color_idx =1;
static gboolean g_figlet_mode=FALSE;

/* widgets */
static GtkWidget      *g_window       =NULL;
static GtkWidget      *g_textview     =NULL;
static GtkWidget      *g_entry        =NULL;
static GtkWidget      *g_statusbar    =NULL;
static GtkWidget      *g_spinner      =NULL;
static GtkWidget      *g_btn_gen      =NULL;
static GtkWidget      *g_btn_model[3];
static GtkWidget      *g_btn_copy     =NULL;
static GtkWidget      *g_btn_clear    =NULL;
static GtkWidget      *g_label_model  =NULL;
static GtkWidget      *g_theme_switch =NULL;
static GtkWidget      *g_color_dd     =NULL;  /* GtkDropDown */
static GtkWidget      *g_prompt_dd    =NULL;  /* GtkDropDown */
static GtkWidget      *g_font_dd      =NULL;  /* GtkDropDown */
static GtkWidget      *g_prompt_edit  =NULL;
static GtkWidget      *g_btn_save_prm =NULL;
static GtkWidget      *g_btn_reload   =NULL;
static GtkWidget      *g_radio_model  =NULL;
static GtkWidget      *g_radio_figlet =NULL;
static GtkWidget      *g_model_row    =NULL;
static GtkWidget      *g_font_row     =NULL;
static GtkWidget      *g_prompt_panel =NULL;
static GtkCssProvider *g_css          =NULL;

/* string lists for dropdowns */
static GtkStringList  *g_color_list   =NULL;
static GtkStringList  *g_prompt_list  =NULL;
static GtkStringList  *g_font_list    =NULL;

/* CSS */
static void apply_css(void) {
    const char *ac=(g_color_idx>0&&g_color_idx<N_COLORS)?COLORS[g_color_idx].css_color:NULL;
    const char *bg  =g_dark_mode?"#1e1e1e":"#ffffff";
    const char *tvbg=g_dark_mode?"#121212":"#f8f8f8";
    const char *fg  =g_dark_mode?"#dddddd":"#111111";
    char css[2048];
    snprintf(css,sizeof(css),
        "window{background-color:%s;color:%s;}"
        "textview,textview text{"
        "  font-family:'Monospace','Courier New',monospace;"
        "  font-size:13px;background-color:%s;color:%s;}"
        ".title-label{font-size:17px;font-weight:bold;}",
        bg,fg,tvbg,ac?ac:fg);
    gtk_css_provider_load_from_string(g_css,css);
}

static void set_status(const char *msg){gtk_label_set_text(GTK_LABEL(g_statusbar),msg);}
static void update_model_label(void){gtk_label_set_text(GTK_LABEL(g_label_model),g_model_name[g_current_slot-1]);}
static void set_text(const char *t){GtkTextBuffer *b=gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview));gtk_text_buffer_set_text(b,t,-1);}
static void set_busy(gboolean busy) {
    if (busy){gtk_spinner_start(GTK_SPINNER(g_spinner));gtk_widget_set_sensitive(g_btn_gen,FALSE);gtk_widget_set_sensitive(g_entry,FALSE);}
    else     {gtk_spinner_stop(GTK_SPINNER(g_spinner)); gtk_widget_set_sensitive(g_btn_gen,TRUE); gtk_widget_set_sensitive(g_entry,TRUE);gtk_widget_grab_focus(g_entry);}
}
static void update_prompt_editor(void) {
    GtkTextBuffer *b=gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_prompt_edit));
    gtk_text_buffer_set_text(b,g_current_sysprompt,-1);
}

/* rebuild prompt dropdown */
static void rebuild_prompt_list(void) {
    /* clear */
    while (g_list_model_get_n_items(G_LIST_MODEL(g_prompt_list))>0)
        gtk_string_list_remove(g_prompt_list,0);
    gtk_string_list_append(g_prompt_list,"Built-in (default)");
    for (int i=0;i<g_n_prompts;i++)
        gtk_string_list_append(g_prompt_list,g_prompts[i].name);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_prompt_dd),(guint)g_prompt_idx);
}

static void update_mode_ui(void) {
    gboolean fig=g_figlet_mode;
    gtk_widget_set_sensitive(g_model_row,   !fig);
    gtk_widget_set_sensitive(g_prompt_panel, !fig);
    gtk_widget_set_sensitive(g_font_row,      fig);
    gtk_widget_set_sensitive(g_spinner,      !fig);
}

/* poll IPC */
static gboolean poll_ipc_cb(gpointer ud) {
    (void)ud;
    if (g_ipc_fd_read<0) {
        ipc_connect();
        if (g_ipc_fd_read<0) {
            struct stat st;
            if (stat(DONE_FLAG,&st)==0) {
                FILE *f=fopen(RESULT_FILE,"r");
                if (f){char *buf=NULL;size_t sz=0;ssize_t n=getdelim(&buf,&sz,'\0',f);fclose(f);if(n>0)set_text(buf);free(buf);}
                remove(DONE_FLAG);
                g_state=ST_DONE;set_busy(FALSE);set_status("Done! Ctrl+A, Ctrl+C to copy.");
                g_poll_timer=0;return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }
    }
    static char ibuf[65536];static int ibuf_pos=0;
    char c;
    while (read(g_ipc_fd_read,&c,1)==1) {
        if (c=='\n') {
            ibuf[ibuf_pos]='\0';ibuf_pos=0;
            char *rp=strstr(ibuf,"\"rsp\":");if(!rp)continue;
            int rsp=atoi(rp+6);
            if (rsp==1) {
                char *tok=strstr(ibuf,"\"token\":\"");
                if (tok){tok+=9;char token[4096];int tl=json_unescape(tok,token,(int)sizeof(token));
                    if(g_accum_len+tl<(int)sizeof(g_accum)-1){memcpy(g_accum+g_accum_len,token,(size_t)tl);g_accum_len+=tl;g_accum[g_accum_len]='\0';}
                    set_text(g_accum);}
            } else if (rsp==2) {
                const char *res=NULL;char full[65536];full[0]='\0';
                char *txt=strstr(ibuf,"\"text\":\"");
                if(txt){txt+=8;json_unescape(txt,full,(int)sizeof(full));res=full;}
                else if(g_accum_len>0)res=g_accum;
                if(res&&*res){set_text(res);FILE *rf=fopen(RESULT_FILE,"w");if(rf){fputs(res,rf);fclose(rf);}}
                g_accum_len=0;g_accum[0]='\0';
                g_state=ST_DONE;set_busy(FALSE);set_status("Done! Ctrl+A, Ctrl+C to copy.");
                g_poll_timer=0;return G_SOURCE_REMOVE;
            } else if (rsp==11) {
                char em[256]="LLM error";char *err=strstr(ibuf,"\"error\":\"");
                if(err)json_unescape(err+9,em,(int)sizeof(em));
                g_state=ST_ERROR;set_busy(FALSE);
                char st[320];snprintf(st,sizeof(st),"Error: %.250s",em);set_status(st);
                g_poll_timer=0;return G_SOURCE_REMOVE;
            } else if (rsp==31) {
                char *vend=strstr(ibuf,"\"vendor\":\"");char *modl=strstr(ibuf,"\"model\":\"");
                if(vend&&modl){char vendor[VENDOR_MAX]="",model[MODEL_NAME_MAX]="";
                    json_unescape(vend+10,vendor,VENDOR_MAX-1);json_unescape(modl+9,model,MODEL_NAME_MAX-1);
                    int s=g_current_slot-1;
                    if(vendor[0])snprintf(g_model_name[s],MODEL_NAME_MAX,"%.60s / %.120s",vendor,model);
                    else         snprintf(g_model_name[s],MODEL_NAME_MAX,"%.180s",model);
                    update_model_label();}
            }
        } else {if(ibuf_pos<(int)sizeof(ibuf)-1)ibuf[ibuf_pos++]=c;}
    }
    return G_SOURCE_CONTINUE;
}

/* generate */
static void do_generate(void) {
    const char *prompt=gtk_editable_get_text(GTK_EDITABLE(g_entry));
    if (!prompt||!*prompt){set_status("Enter text first.");return;}
    set_text("");g_accum_len=0;g_accum[0]='\0';

    if (g_figlet_mode) {
        guint fidx=gtk_drop_down_get_selected(GTK_DROP_DOWN(g_font_dd));
        /* 0=default, 1..n = fonts */
        const char *font=(fidx>0&&(int)fidx<=g_n_fonts)?g_fonts[fidx-1]:NULL;
        char *result=run_figlet(prompt,font);
        if (result&&*result) {set_text(result);set_status("Done! Ctrl+A, Ctrl+C to copy.");}
        else set_status("Error: figlet failed. Is figlet installed?");
        free(result);
        return;
    }

    /* model mode - read prompt from editor */
    GtkTextBuffer *pb=gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_prompt_edit));
    GtkTextIter ps,pe;gtk_text_buffer_get_bounds(pb,&ps,&pe);
    char *edited=gtk_text_buffer_get_text(pb,&ps,&pe,FALSE);
    strncpy(g_current_sysprompt,edited,SYSPROMPT_MAX-1);
    g_current_sysprompt[SYSPROMPT_MAX-1]='\0';
    g_free(edited);

    remove(DONE_FLAG);
    FILE *f=fopen(INPUT_FILE,"w");if(f){fprintf(f,"%s\n",prompt);fclose(f);}
    f=fopen(RESULT_FILE,"w");if(f)fclose(f);
    g_state=ST_WAITING;set_busy(TRUE);
    char st[256];snprintf(st,sizeof(st),"Generating with %.190s...",g_model_name[g_current_slot-1]);
    set_status(st);
    if (g_ipc_fd_write>=0) {
        ipc_set_provider(g_current_slot);
        ipc_send_sysprompt();
        ipc_send_prompt(prompt,g_current_slot);
    } else {
        char cmd[512];snprintf(cmd,sizeof(cmd),"edi %s &",WORKFLOW);(void)system(cmd);
    }
    if (!g_poll_timer) g_poll_timer=g_timeout_add(50,poll_ipc_cb,NULL);
}

/* callbacks */
static void on_generate(GtkButton *b,gpointer d)      {(void)b;(void)d;do_generate();}
static void on_entry_activate(GtkEntry *e,gpointer d) {(void)e;(void)d;do_generate();}
static void on_copy(GtkButton *b,gpointer d) {
    (void)b;(void)d;
    GtkTextBuffer *buf=gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_textview));
    GtkTextIter s,e;gtk_text_buffer_get_bounds(buf,&s,&e);
    char *text=gtk_text_buffer_get_text(buf,&s,&e,FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(g_textview),text);
    g_free(text);set_status("Copied to clipboard!");
}
static void on_clear(GtkButton *b,gpointer d) {
    (void)b;(void)d;
    set_text("");gtk_editable_set_text(GTK_EDITABLE(g_entry),"");
    set_status("Ready.");g_state=ST_IDLE;
}
static void on_model_btn(GtkButton *b,gpointer data) {
    (void)b;g_current_slot=GPOINTER_TO_INT(data);update_model_label();
    if (g_ipc_fd_write>=0){
        ipc_set_provider(g_current_slot);
        char json[64];snprintf(json,sizeof(json),"{\"v\":1,\"id\":\"%d\",\"cmd\":32}",g_ipc_seq++);
        ipc_send_raw(json);
    }
    char st[256];snprintf(st,sizeof(st),"Model %d: %.190s",g_current_slot,g_model_name[g_current_slot-1]);
    set_status(st);
}
static void on_theme_switch(GObject *obj,GParamSpec *ps,gpointer d) {
    (void)ps;(void)d;
    g_dark_mode=gtk_switch_get_active(GTK_SWITCH(obj));
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",g_dark_mode,NULL);
    apply_css();
}
static void on_color_changed(GObject *obj,GParamSpec *ps,gpointer d) {
    (void)ps;(void)d;
    g_color_idx=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    apply_css();
}
static void on_prompt_selected(GObject *obj,GParamSpec *ps,gpointer d) {
    (void)ps;(void)d;
    g_prompt_idx=(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    load_sysprompt(g_prompt_idx);update_prompt_editor();
    if (g_prompt_idx==0) set_status("System prompt: Built-in");
    else {char st[256];snprintf(st,sizeof(st),"System prompt: %s",g_prompts[g_prompt_idx-1].name);set_status(st);}
}
static void on_reload_prompts(GtkButton *b,gpointer d) {
    (void)b;(void)d;scan_sysprompts();rebuild_prompt_list();
    char st[64];snprintf(st,sizeof(st),"Reloaded: %d prompts",g_n_prompts);set_status(st);
}
static void on_mode_changed(GtkCheckButton *btn,gpointer d) {
    (void)btn;(void)d;
    g_figlet_mode=gtk_check_button_get_active(GTK_CHECK_BUTTON(g_radio_figlet));
    update_mode_ui();
    set_status(g_figlet_mode?"Figlet mode: type text and choose a font."
                             :"Model mode: LLM generates ASCII art.");
}

/* save prompt dialog - use plain GtkWindow */
static void on_save_do(GtkButton *b,gpointer ne) {
    (void)b;
    const char *nm=gtk_editable_get_text(GTK_EDITABLE(ne));
    if (!nm||!*nm){return;}
    char path[PATH_MAX_LEN];
    snprintf(path,sizeof(path),"%.400s/%.255s.prm",g_sysprompt_dir,nm);
    GtkTextBuffer *pb=gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_prompt_edit));
    GtkTextIter ps,pe;gtk_text_buffer_get_bounds(pb,&ps,&pe);
    char *text=gtk_text_buffer_get_text(pb,&ps,&pe,FALSE);
    FILE *f=fopen(path,"w");
    if (f){fputs(text,f);fclose(f);set_status("Prompt saved!");}
    else set_status("Error: cannot save.");
    g_free(text);
    scan_sysprompts();rebuild_prompt_list();
    /* close parent window */
    GtkWidget *win=gtk_widget_get_ancestor(GTK_WIDGET(b),GTK_TYPE_WINDOW);
    if (win) gtk_window_destroy(GTK_WINDOW(win));
}
static void on_save_prompt(GtkButton *b,gpointer d) {
    (void)b;(void)d;
    GtkWidget *win=gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win),"Save prompt as...");
    gtk_window_set_transient_for(GTK_WINDOW(win),GTK_WINDOW(g_window));
    gtk_window_set_modal(GTK_WINDOW(win),TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win),400,80);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_widget_set_margin_start(box,12);gtk_widget_set_margin_end(box,12);
    gtk_widget_set_margin_top(box,12);gtk_widget_set_margin_bottom(box,12);
    gtk_box_append(GTK_BOX(box),gtk_label_new("Name (without .prm):"));
    GtkWidget *ne=gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ne),"my_prompt");
    gtk_widget_set_hexpand(ne,TRUE);
    gtk_box_append(GTK_BOX(box),ne);
    GtkWidget *btn=gtk_button_new_with_label("Save");
    gtk_box_append(GTK_BOX(box),btn);
    g_signal_connect(btn,"clicked",G_CALLBACK(on_save_do),ne);
    gtk_window_set_child(GTK_WINDOW(win),box);
    gtk_window_present(GTK_WINDOW(win));
}

/* build UI */
static void build_ui(GtkApplication *app) {
    g_window=gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_window),"nomo - ASCII Art Generator");
    gtk_window_set_default_size(GTK_WINDOW(g_window),960,800);

    g_css=gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(g_css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",TRUE,NULL);
    apply_css();

    GtkWidget *vpaned=gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_wide_handle(GTK_PANED(vpaned),TRUE);
    gtk_window_set_child(GTK_WINDOW(g_window),vpaned);

    /* top panel */
    GtkWidget *top=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    gtk_widget_set_margin_start(top,10);gtk_widget_set_margin_end(top,10);
    gtk_widget_set_margin_top(top,8);gtk_widget_set_margin_bottom(top,4);
    gtk_paned_set_start_child(GTK_PANED(vpaned),top);
    gtk_paned_set_resize_start_child(GTK_PANED(vpaned),TRUE);

    /* row1: title + color + theme */
    GtkWidget *row1=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    gtk_box_append(GTK_BOX(top),row1);
    GtkWidget *title=gtk_label_new("nomo");
    gtk_widget_add_css_class(title,"title-label");
    gtk_widget_set_halign(title,GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row1),title);
    GtkWidget *sp=gtk_label_new("");gtk_widget_set_hexpand(sp,TRUE);gtk_box_append(GTK_BOX(row1),sp);

    gtk_box_append(GTK_BOX(row1),gtk_label_new("Color:"));
    g_color_list=gtk_string_list_new(NULL);
    for (int i=0;i<N_COLORS;i++) gtk_string_list_append(g_color_list,COLORS[i].name);
    /* gtk_string_list_new needs NULL-terminated array */
    g_color_dd=gtk_drop_down_new(G_LIST_MODEL(g_color_list),NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_color_dd),(guint)g_color_idx);
    gtk_box_append(GTK_BOX(row1),g_color_dd);
    g_signal_connect(g_color_dd,"notify::selected",G_CALLBACK(on_color_changed),NULL);

    gtk_box_append(GTK_BOX(row1),gtk_label_new("Dark:"));
    g_theme_switch=gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(g_theme_switch),TRUE);
    gtk_widget_set_valign(g_theme_switch,GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row1),g_theme_switch);
    g_signal_connect(g_theme_switch,"notify::active",G_CALLBACK(on_theme_switch),NULL);

    /* row2: mode radio + models/font */
    GtkWidget *row2=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    gtk_box_append(GTK_BOX(top),row2);

    g_radio_model=gtk_check_button_new_with_label("Model");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_radio_model),TRUE);
    g_radio_figlet=gtk_check_button_new_with_label("Figlet");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(g_radio_figlet),GTK_CHECK_BUTTON(g_radio_model));
    gtk_box_append(GTK_BOX(row2),g_radio_model);
    gtk_box_append(GTK_BOX(row2),g_radio_figlet);
    g_signal_connect(g_radio_model, "toggled",G_CALLBACK(on_mode_changed),NULL);
    g_signal_connect(g_radio_figlet,"toggled",G_CALLBACK(on_mode_changed),NULL);
    gtk_box_append(GTK_BOX(row2),gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    g_model_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_append(GTK_BOX(row2),g_model_row);
    gtk_box_append(GTK_BOX(g_model_row),gtk_label_new("Model:"));
    for (int s=0;s<3;s++) {
        g_btn_model[s]=gtk_button_new_with_label(g_model_short[s]);
        g_signal_connect(g_btn_model[s],"clicked",G_CALLBACK(on_model_btn),GINT_TO_POINTER(s+1));
        gtk_box_append(GTK_BOX(g_model_row),g_btn_model[s]);
    }
    g_label_model=gtk_label_new("");
    gtk_widget_set_halign(g_label_model,GTK_ALIGN_END);
    gtk_widget_set_hexpand(g_label_model,TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_label_model),PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(g_model_row),g_label_model);
    update_model_label();

    /* figlet font row */
    g_font_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_append(GTK_BOX(row2),g_font_row);
    gtk_box_append(GTK_BOX(g_font_row),gtk_label_new("Font:"));

    /* build font string list */
    g_font_list=gtk_string_list_new(NULL);
    gtk_string_list_append(g_font_list,"default");
    for (int i=0;i<g_n_fonts;i++) gtk_string_list_append(g_font_list,g_fonts[i]);
    g_font_dd=gtk_drop_down_new(G_LIST_MODEL(g_font_list),NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_font_dd),0);
    gtk_widget_set_hexpand(g_font_dd,TRUE);
    gtk_box_append(GTK_BOX(g_font_row),g_font_dd);
    gtk_widget_set_sensitive(g_font_row,FALSE);

    gtk_box_append(GTK_BOX(top),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    /* row3: prompt entry */
    GtkWidget *row3=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_append(GTK_BOX(top),row3);
    gtk_box_append(GTK_BOX(row3),gtk_label_new("Prompt:"));
    g_entry=gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry),"a dragon, HELLO, нарисуй кота...");
    gtk_widget_set_hexpand(g_entry,TRUE);
    gtk_box_append(GTK_BOX(row3),g_entry);
    g_signal_connect(g_entry,"activate",G_CALLBACK(on_entry_activate),NULL);
    g_btn_gen=gtk_button_new_with_label("Generate");
    gtk_box_append(GTK_BOX(row3),g_btn_gen);
    g_signal_connect(g_btn_gen,"clicked",G_CALLBACK(on_generate),NULL);
    g_spinner=gtk_spinner_new();
    gtk_box_append(GTK_BOX(row3),g_spinner);

    /* result textview */
    GtkWidget *scroll=gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll,TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(top),scroll);
    g_textview=gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_textview),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_textview),GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(g_textview),10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(g_textview),10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(g_textview),8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(g_textview),8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),g_textview);
    set_text("  nomo - ASCII Art Generator\n\n"
             "  [ Model ] - LLM generates ASCII art\n"
             "  [ Figlet ] - render text with a local font\n\n"
             "  Examples:\n"
             "    a dragon breathing fire\n"
             "    Linux   (in Figlet mode)\n");

    /* bottom button row */
    GtkWidget *bot=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_append(GTK_BOX(top),bot);
    g_btn_copy=gtk_button_new_with_label("Copy All");
    g_signal_connect(g_btn_copy,"clicked",G_CALLBACK(on_copy),NULL);
    gtk_box_append(GTK_BOX(bot),g_btn_copy);
    g_btn_clear=gtk_button_new_with_label("Clear");
    g_signal_connect(g_btn_clear,"clicked",G_CALLBACK(on_clear),NULL);
    gtk_box_append(GTK_BOX(bot),g_btn_clear);
    g_statusbar=gtk_label_new("Ready.");
    gtk_widget_set_halign(g_statusbar,GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_statusbar,TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_statusbar),PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(bot),g_statusbar);

    /* bottom panel - prompt editor */
    g_prompt_panel=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    gtk_widget_set_margin_start(g_prompt_panel,10);gtk_widget_set_margin_end(g_prompt_panel,10);
    gtk_widget_set_margin_top(g_prompt_panel,4);gtk_widget_set_margin_bottom(g_prompt_panel,8);
    gtk_paned_set_end_child(GTK_PANED(vpaned),g_prompt_panel);
    gtk_paned_set_resize_end_child(GTK_PANED(vpaned),FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(vpaned),FALSE);

    GtkWidget *prow=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_append(GTK_BOX(g_prompt_panel),prow);
    gtk_box_append(GTK_BOX(prow),gtk_label_new("System Prompt:"));

    g_prompt_list=gtk_string_list_new(NULL);
    gtk_string_list_append(g_prompt_list,"Built-in (default)");
    for (int i=0;i<g_n_prompts;i++) gtk_string_list_append(g_prompt_list,g_prompts[i].name);
    g_prompt_dd=gtk_drop_down_new(G_LIST_MODEL(g_prompt_list),NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(g_prompt_dd),0);
    gtk_widget_set_hexpand(g_prompt_dd,TRUE);
    gtk_box_append(GTK_BOX(prow),g_prompt_dd);
    g_signal_connect(g_prompt_dd,"notify::selected",G_CALLBACK(on_prompt_selected),NULL);

    g_btn_reload=gtk_button_new_with_label("Reload");
    g_signal_connect(g_btn_reload,"clicked",G_CALLBACK(on_reload_prompts),NULL);
    gtk_box_append(GTK_BOX(prow),g_btn_reload);
    g_btn_save_prm=gtk_button_new_with_label("Save as...");
    g_signal_connect(g_btn_save_prm,"clicked",G_CALLBACK(on_save_prompt),NULL);
    gtk_box_append(GTK_BOX(prow),g_btn_save_prm);

    char di[PATH_MAX_LEN+8];
    snprintf(di,sizeof(di),"  %s",g_sysprompt_dir);
    GtkWidget *dlbl=gtk_label_new(di);
    gtk_widget_set_halign(dlbl,GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(dlbl),PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class(dlbl,"dim-label");
    gtk_box_append(GTK_BOX(g_prompt_panel),dlbl);

    GtkWidget *pscroll=gtk_scrolled_window_new();
    gtk_widget_set_vexpand(pscroll,TRUE);
    gtk_widget_set_size_request(pscroll,-1,140);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pscroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(g_prompt_panel),pscroll);
    g_prompt_edit=gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_prompt_edit),TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_prompt_edit),GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(g_prompt_edit),8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(g_prompt_edit),8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(g_prompt_edit),6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pscroll),g_prompt_edit);
    update_prompt_editor();

    gtk_paned_set_position(GTK_PANED(vpaned),520);
    gtk_window_present(GTK_WINDOW(g_window));

    ipc_connect();
    if (g_ipc_fd_write>=0) {
        ipc_set_provider(g_current_slot);
        char json[64];snprintf(json,sizeof(json),"{\"v\":1,\"id\":\"%d\",\"cmd\":32}",g_ipc_seq++);
        ipc_send_raw(json);
        g_timeout_add(100,poll_ipc_cb,NULL);
        set_status("Connected to mshell IPC. Ready.");
    }
}

static void on_activate(GtkApplication *app,gpointer d) {
    (void)d;
    load_model_names();
    ensure_sysprompt_dir();
    scan_sysprompts();
    scan_figlet_fonts();
    load_sysprompt(0);
    build_ui(app);
}

int main(int argc,char *argv[]) {
    GtkApplication *app=gtk_application_new("art2dec.nomo",G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app,"activate",G_CALLBACK(on_activate),NULL);
    int rc=g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);
    return rc;
}
