/*____________________________________________________________________________
        
        gkrellm multiping plugin

        Copyright (C) 2002 Jindrich Makovicka

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, Write to the Free Software
        Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
____________________________________________________________________________*/

#include <stdio.h>
#include <string.h>
#include <gkrellm2/gkrellm.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "decal_multiping_status.xpm"

#define	CONFIG_NAME	"Multiping"
#define	STYLE_NAME	"multiping"

#define COMMAND "/usr/local/lib/gkrellm2/plugins/pinger"

static GkrellmMonitor *monitor;
static GkrellmPanel *panel;

static gint style_id;

static FILE *pinger_pipe = NULL;
static pid_t pinger_pid;

static gshort selected_row;

static gboolean delete_list;
static gboolean list_modified;

static GtkWidget *plugin_vbox;
static GtkWidget *label_entry;
static GtkWidget *url_entry;
static GtkWidget *updatefreq_spin;
static GtkWidget *multiping_clist;
static GtkWidget *show_trip_checkbutton;
static GtkWidget *dynamic_checkbutton;

static GkrellmPiximage *decal_status_image;
static GdkPixmap *status_pixmap;
static GdkBitmap *status_mask;

static gint vspacing, hspacing, time_xoffset;

static gint helper_err = 0;

typedef struct _host_data {
    GString *name, *ip, *percentage, *sent_str, *recv_str, *msg, *shortmsg, *updatefreq;
    GkrellmDecal *name_text, *msg_text, *decal_pix;
    gboolean show_trip, dynamic;
} host_data;

static GList *hosts;

static host_data *host_malloc()
{
    host_data *h = (host_data *) g_malloc(sizeof(host_data));
    h->name_text = NULL;
    h->msg_text = NULL;
    h->decal_pix = NULL;
    h->name = g_string_new(NULL);
    h->ip = g_string_new(NULL);
    h->percentage = g_string_new(NULL);
    h->sent_str = g_string_new(NULL);
    h->recv_str = g_string_new(NULL);
    h->msg = g_string_new(NULL);
    h->shortmsg = g_string_new("wait");
    h->updatefreq = g_string_new(NULL);
    h->show_trip = 0;
    h->dynamic = 0;
    return h;
}

static void host_free(host_data * h)
{
    if (h->name_text) {
	gkrellm_destroy_decal(h->name_text);
    }
    if (h->msg_text) {
	gkrellm_destroy_decal(h->msg_text);
    }
    if (h->decal_pix) {
	gkrellm_destroy_decal(h->decal_pix);
    }
    g_string_free(h->name, TRUE);
    g_string_free(h->ip, TRUE);
    g_string_free(h->percentage, TRUE);
    g_string_free(h->sent_str, TRUE);
    g_string_free(h->recv_str, TRUE);
    g_string_free(h->msg, TRUE);
    g_string_free(h->shortmsg, TRUE);
    g_string_free(h->updatefreq, TRUE);
    g_free(h);
}

static void strip_nl(gchar *buf)
{
    if (buf[strlen(buf) - 1] == '\n')
	buf[strlen(buf) - 1] = 0;
}


static void host_read_pipe(host_data * h)
{
    gchar buf[512];

    char* res = fgets(buf, 512, pinger_pipe);
    if (res == 0) {
	helper_err = 1;
	return;
    }
    strip_nl(buf);
    g_string_assign(h->percentage, buf);
    fgets(buf, 512, pinger_pipe);
    strip_nl(buf);
    g_string_assign(h->sent_str, buf);
    fgets(buf, 512, pinger_pipe);
    strip_nl(buf);
    g_string_assign(h->recv_str, buf);
    fgets(buf, 512, pinger_pipe);
    strip_nl(buf);
    g_string_assign(h->msg, buf);
    fgets(buf, 512, pinger_pipe);
    strip_nl(buf);
    g_string_assign(h->shortmsg, buf);
}

static void kill_pinger()
{
    if (pinger_pipe) {
	kill(pinger_pid, SIGTERM);
	waitpid(pinger_pid, NULL, 0);
	fclose(pinger_pipe);
	pinger_pipe = NULL;
    }
}

static void launch_pipe()
{
    GString *s = g_string_new(COMMAND);
    gint i;
    GList *list;
    host_data *h;
    pid_t pid;
    gint mypipe[2];

    for (i = 0, list = hosts; list; list = list->next, i++) {
	h = (host_data *) list->data;
	g_string_append(s, " ");
	g_string_append(s, h->ip->str);
	g_string_append(s, " ");
	g_string_append(s, h->updatefreq->str);
	g_string_append(s, " ");
	g_string_append(s, h->dynamic ? "1" : "0");
    }

    if (pipe(mypipe)) {
	fprintf(stderr, "Pipe failed.\n");
	return;
    }

    pid = fork();
    if (pid == 0) {
	/* This is the child process.  Execute the shell command. */
	close(mypipe[0]);
	dup2(mypipe[1], 1);
	execl("/bin/sh", "/bin/sh", "-c", s->str, NULL);
	_exit(EXIT_FAILURE);
    } else if (pid < 0) {
	/* The fork failed.  Report failure.  */
	fprintf(stderr, "failed to fork\n");
    } else {
	/* This is the parent process. Prepare the pipe stream. */
	close(mypipe[1]);
	pinger_pipe = fdopen(mypipe[0], "r");
	pinger_pid = pid;
    }
}

/*
static void
host_append_info(host_data *h, GString *str)
{
  GString *s = g_string_new(NULL);

  g_string_sprintf(s, "%s: %s, %s\n",
		   h->name->str, h->percentage->str, h->msg->str);
  g_string_append(str, s->str);

  g_string_free(s, TRUE);
}
*/

static void host_draw_name(host_data * h)
{
    gkrellm_draw_decal_text(panel, h->name_text, h->name->str, -1);
}

static void host_draw_msg(host_data * h)
{
    gint n, p;

    if (h->show_trip) {
	gkrellm_draw_decal_text(panel, h->msg_text, h->shortmsg->str, -1);
    }
    n = sscanf(h->percentage->str, "%d", &p);
    if (n != 1 || p == 0) {
	gkrellm_draw_decal_pixmap(panel, h->decal_pix, 0);
    } else if (p < 100) {
	gkrellm_draw_decal_pixmap(panel, h->decal_pix, 2);
    } else {
	gkrellm_draw_decal_pixmap(panel, h->decal_pix, 1);
    }
}

static GList *append_host(GList * list, gchar * name, gchar * ip, gboolean show_trip, gboolean dynamic, gchar * updatefreq)
{
    host_data *h = host_malloc();
    g_string_assign(h->name, name);
    g_string_assign(h->ip, ip);
    h->show_trip = show_trip;
    h->dynamic = dynamic;
    g_string_assign(h->updatefreq, updatefreq);
    return g_list_append(list, h);
}

static gint
display_host(host_data * h, GkrellmStyle * style, GkrellmTextstyle * ts,
	     GkrellmTextstyle * ts_alt, gint y)
{
    if (h->show_trip) {
	h->msg_text =
	    gkrellm_create_decal_text(panel, "999", ts_alt, style, 0, y, 0);

	h->msg_text->x = gkrellm_chart_width() - h->msg_text->w + time_xoffset;
    }
    
    h->decal_pix =
	gkrellm_create_decal_pixmap(panel, status_pixmap, status_mask, 3,
				    style, -1, y);
    h->name_text =
	gkrellm_create_decal_text(panel, "Ay", ts, style,
				  h->decal_pix->x+h->decal_pix->w+hspacing, y, -1);

    if (h->name_text->h < h->decal_pix->h) {
	h->name_text->y += (h->decal_pix->h-h->name_text->h)/2;
	if (h->show_trip) {
	    h->msg_text->y = h->name_text->y;
	}
	return h->decal_pix->y + h->decal_pix->h + vspacing;
    } else {
	h->decal_pix->y += (h->name_text->h-h->decal_pix->h)/2;
	return h->name_text->y + h->name_text->h + vspacing;
    }
}

static gint panel_click_event(GtkWidget * widget, GdkEventButton * ev)
{
    if (ev->button == 3)
	gkrellm_open_config_window(monitor);

    return 1;
}

static gint panel_expose_event(GtkWidget * widget, GdkEventExpose * ev)
{
    gdk_draw_pixmap(widget->window,
		    widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		    panel->pixmap, ev->area.x, ev->area.y, ev->area.x,
		    ev->area.y, ev->area.width, ev->area.height);
    return FALSE;
}


static void update_plugin()
{
    fd_set fds;
    struct timeval tv;
    gint ret;
    GString *str = g_string_new(NULL);

    FD_ZERO(&fds);
    FD_SET(fileno(pinger_pipe), &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    ret = select(fileno(pinger_pipe) + 1, &fds, 0, 0, &tv);

    if (ret) {
	g_list_foreach(hosts, (GFunc) host_read_pipe, NULL);
	g_list_foreach(hosts, (GFunc) host_draw_msg, NULL);
	gkrellm_draw_panel_layers(panel);
    }

    if (helper_err) {
	kill_pinger();
	launch_pipe();
	helper_err = 0;
    }
    
    /*  
       if (GK.minute_tick && tooltip) {
       //    if (tooltip->tip_window == NULL || !GTK_WIDGET_VISIBLE(tooltip->tip_window)) {
       g_list_foreach(hosts, (GFunc)host_append_info, str);
       gtk_tooltips_set_tip(tooltip, panel->drawing_area, str->str, NULL);
       gtk_tooltips_set_delay(tooltip, 750);
       gtk_tooltips_enable(tooltip);
       //    }
       }
     */
    g_string_free(str, TRUE);
}

static void setup_display(gboolean first_create)
{
    gshort i;
    gint y;
    GkrellmStyle *style;
    GkrellmTextstyle *ts, *ts_alt;
    GList *list;
    host_data *h;

    if (first_create) {
	panel = gkrellm_panel_new0();
    }

    style = gkrellm_panel_style(style_id);
    ts = gkrellm_meter_textstyle(style_id);
    ts_alt = gkrellm_meter_alt_textstyle(style_id);

    y = 3;
//    y = style->top_margin;
    
    for (i = 0, list = hosts; list; list = list->next, i++) {
	h = (host_data *) list->data;
	y = display_host(h, style, ts, ts_alt, y);
    }

    gkrellm_panel_configure(panel, NULL, style);
    gkrellm_panel_create(plugin_vbox, monitor, panel);

    if (first_create) {
	gtk_signal_connect(GTK_OBJECT(panel->drawing_area), "expose_event",
			   (GtkSignalFunc) panel_expose_event, NULL);
	gtk_signal_connect(GTK_OBJECT(panel->drawing_area),
			   "button_release_event",
			   (GtkSignalFunc) panel_click_event, NULL);
    }

    g_list_foreach(hosts, (GFunc) host_draw_name, NULL);
    g_list_foreach(hosts, (GFunc) host_draw_msg, NULL);

    gkrellm_draw_panel_layers(panel);
}

static void create_plugin(GtkWidget * vbox, gint first_create)
{
    plugin_vbox = vbox;

    gkrellm_load_piximage("decal_multiping_status", decal_multiping_status_xpm,
			  &decal_status_image, STYLE_NAME);

    gkrellm_scale_piximage_to_pixmap(decal_status_image, &status_pixmap,
				     &status_mask, 0, 0);

    if (!gkrellm_get_gkrellmrc_integer("multiping_vspacing", &vspacing))
	vspacing = 2;
    if (!gkrellm_get_gkrellmrc_integer("multiping_hspacing", &hspacing))
	hspacing = 2;
    if (!gkrellm_get_gkrellmrc_integer("multiping_time_xoffset", &time_xoffset))
	time_xoffset = 0;

    kill_pinger();
    launch_pipe();
    setup_display(first_create);
}

static void cb_up(GtkWidget * widget, gpointer drawer)
{
    GtkWidget *clist;
    gshort row;

    clist = multiping_clist;
    row = selected_row;
    if (row > 0) {
	gtk_clist_row_move(GTK_CLIST(clist), row, row - 1);
	gtk_clist_select_row(GTK_CLIST(clist), row - 1, -1);
	if (gtk_clist_row_is_visible(GTK_CLIST(clist), row - 1) !=
	    GTK_VISIBILITY_FULL)
	    gtk_clist_moveto(GTK_CLIST(clist), row - 1, -1, 0.0, 0.0);
	selected_row = row - 1;
	list_modified = TRUE;
    }
}


static void cb_down(GtkWidget * widget, gpointer drawer)
{
    GtkWidget *clist;
    gshort row;


    clist = multiping_clist;
    row = selected_row;
    if (row >= 0 && row < GTK_CLIST(clist)->rows - 1) {
	gtk_clist_row_move(GTK_CLIST(clist), row, row + 1);
	gtk_clist_select_row(GTK_CLIST(clist), row + 1, -1);
	if (gtk_clist_row_is_visible(GTK_CLIST(clist), row + 1) !=
	    GTK_VISIBILITY_FULL)
	    gtk_clist_moveto(GTK_CLIST(clist), row + 1, -1, 1.0, 0.0);
	selected_row = row + 1;
	list_modified = TRUE;
    }
}


static void reset_entries()
{
    gtk_entry_set_text(GTK_ENTRY(label_entry), "");
    gtk_entry_set_text(GTK_ENTRY(url_entry), "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_trip_checkbutton), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dynamic_checkbutton), FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(updatefreq_spin),60);
    return;
}


static void cb_selected(GtkWidget * clist, gint row, gint column,
			GdkEventButton * bevent, gpointer data)
{
    gchar *s;

    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 0, &s);
    gtk_entry_set_text(GTK_ENTRY(label_entry), s);
    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 1, &s);
    gtk_entry_set_text(GTK_ENTRY(url_entry), s);
    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 2, &s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_trip_checkbutton), strcmp(s, "yes") == 0);
    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 3, &s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dynamic_checkbutton), strcmp(s, "yes") == 0);
    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 4, &s);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(updatefreq_spin),atoi(s));

    selected_row = row;

    return;
}


static void cb_unselected(GtkWidget * clist, gint row, gint column,
			  GdkEventButton * bevent, gpointer data)
{
    selected_row = -1;
    reset_entries();

    return;
}


static void cb_enter(GtkWidget * widget, gpointer data)
{
    gchar *buf[5];
    gboolean active;

    buf[0] = gkrellm_gtk_entry_get_text(&label_entry);
    buf[1] = gkrellm_gtk_entry_get_text(&url_entry);
    active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(show_trip_checkbutton));
    buf[2] = active ? "yes" : "no";
    active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dynamic_checkbutton));
    buf[3] = active ? "yes" : "no";
    buf[4] = gkrellm_gtk_entry_get_text(&updatefreq_spin);
    if ((strlen(buf[0]) == 0) || (strlen(buf[1]) == 0))
	return;

    if (selected_row >= 0) {
	gtk_clist_set_text(GTK_CLIST(multiping_clist), selected_row, 0,
			   buf[0]);
	gtk_clist_set_text(GTK_CLIST(multiping_clist), selected_row, 1,
			   buf[1]);
	gtk_clist_set_text(GTK_CLIST(multiping_clist), selected_row, 2,
			   buf[2]);
	gtk_clist_set_text(GTK_CLIST(multiping_clist), selected_row, 3,
			   buf[3]);
	gtk_clist_set_text(GTK_CLIST(multiping_clist), selected_row, 4,
			   buf[4]);
	gtk_clist_unselect_row(GTK_CLIST(multiping_clist), selected_row,
			       0);
	selected_row = -1;
    } else
	gtk_clist_append(GTK_CLIST(multiping_clist), buf);

    reset_entries();
    list_modified = TRUE;

    return;
}


static void cb_delete(GtkWidget * widget, gpointer data)
{
    reset_entries();

    if (selected_row >= 0) {
	gtk_clist_remove(GTK_CLIST(multiping_clist), selected_row);
	list_modified = TRUE;
	selected_row = -1;
    }

    return;
}

static gchar plugin_about_text[] =
    "GKrellM Multiping version " VERSION "\n\n\n"
    "Copyright (C) 2002 by Jindrich Makovicka\n"
    "makovick@kmlinux.fjfi.cvut.cz\n"
    "Released under the GPL.\n"
    "Portions (C) 2001 Tilman \"Trillian\" Sauerbeck";

static void create_plugin_config(GtkWidget * tab_vbox)
{
    GtkWidget *tabs;
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *hbox2;
    GtkWidget *label;
    GtkWidget *scrolled;
    GtkWidget *button;
    GtkWidget *arrow;
    GtkAdjustment *spin_adjust;
    GList *list;
    GtkWidget *info_label;
    gchar *buf[5];
    gchar *titles[5] = { "Label", "Hostname / IP Address", "Trip", "Dynamic", "Ping Freq" };
    gshort i;
    host_data *nt;

    list_modified = FALSE;
    selected_row = -1;

/* Make a couple of tabs */
    tabs = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(tabs), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(tab_vbox), tabs, TRUE, TRUE, 0);

/* Sources tab */
    vbox = gkrellm_gtk_framed_notebook_page(tabs, "Hosts");

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 2);
    label = gtk_label_new("Label:");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 2);
    label_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(label_entry), 25);
//    gtk_widget_set_usize(label_entry, 180, 0);
    gtk_box_pack_start(GTK_BOX(hbox), label_entry, FALSE, TRUE, 0);

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 2);
    label = gtk_label_new("Hostname / IP Address:");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 2);
    url_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(url_entry), 75);
    gtk_widget_set_usize(url_entry, 475, 0);
    gtk_box_pack_start(GTK_BOX(hbox), url_entry, FALSE, TRUE, 2);

    hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 5);
    label = gtk_label_new("Ping Frequency:");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 2);
    spin_adjust = (GtkAdjustment *) gtk_adjustment_new(0,10,3600,1.0,0,0);
    updatefreq_spin = gtk_spin_button_new(spin_adjust,1.0,0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(updatefreq_spin),60);
    gtk_box_pack_start(GTK_BOX(hbox), updatefreq_spin, FALSE, TRUE, 0);
    show_trip_checkbutton = gtk_check_button_new_with_label("Display trip time");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_trip_checkbutton),TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), show_trip_checkbutton, FALSE, TRUE, 0);
    dynamic_checkbutton = gtk_check_button_new_with_label("Dynamic DNS");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dynamic_checkbutton),FALSE);
    gtk_box_pack_start(GTK_BOX(hbox), dynamic_checkbutton, FALSE, TRUE, 0);

    hbox = gtk_hbox_new(TRUE, 100);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 5);

    hbox2 = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(hbox2), GTK_BUTTONBOX_START);
    gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox2), 5);
    gtk_box_pack_start(GTK_BOX(hbox), hbox2, FALSE, FALSE, 5);

    button = gtk_button_new_with_label("Enter");
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) cb_enter, NULL);
    gtk_box_pack_start(GTK_BOX(hbox2), button, TRUE, TRUE, 0);

    button = gtk_button_new_with_label("Delete");
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) cb_delete, NULL);
    gtk_box_pack_start(GTK_BOX(hbox2), button, TRUE, TRUE, 0);

    hbox2 = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(hbox2), GTK_BUTTONBOX_END);
    gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox2), 5);
    gtk_box_pack_start(GTK_BOX(hbox), hbox2, FALSE, FALSE, 5);

    button = gtk_button_new();
    arrow = gtk_arrow_new(GTK_ARROW_UP, GTK_SHADOW_ETCHED_OUT);
    gtk_container_add(GTK_CONTAINER(button), arrow);
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) cb_up, NULL);
    gtk_box_pack_start(GTK_BOX(hbox2), button, TRUE, TRUE, 0);

    button = gtk_button_new();
    arrow = gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_ETCHED_OUT);
    gtk_container_add(GTK_CONTAINER(button), arrow);
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) cb_down, NULL);
    gtk_box_pack_start(GTK_BOX(hbox2), button, TRUE, TRUE, 0);

    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
				   GTK_POLICY_AUTOMATIC,
				   GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    multiping_clist = gtk_clist_new_with_titles(5, titles);
    gtk_clist_set_shadow_type(GTK_CLIST(multiping_clist), GTK_SHADOW_OUT);
    gtk_clist_column_titles_passive(GTK_CLIST(multiping_clist));
    gtk_clist_set_column_justification(GTK_CLIST(multiping_clist), 0,
				       GTK_JUSTIFY_LEFT);
    gtk_clist_set_column_justification(GTK_CLIST(multiping_clist), 1,
				       GTK_JUSTIFY_LEFT);

    gtk_clist_set_column_width(GTK_CLIST(multiping_clist), 0, 100);
    gtk_clist_set_column_width(GTK_CLIST(multiping_clist), 1, 200);

    gtk_signal_connect(GTK_OBJECT(multiping_clist), "select_row",
		       (GtkSignalFunc) cb_selected, NULL);
    gtk_signal_connect(GTK_OBJECT(multiping_clist), "unselect_row",
		       (GtkSignalFunc) cb_unselected, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled), multiping_clist);

    for (i = 0, list = hosts; list; i++, list = list->next) {
	nt = (host_data *) list->data;
	buf[0] = nt->name->str;
	buf[1] = nt->ip->str;
	buf[2] = nt->show_trip ? "yes" : "no";
	buf[3] = nt->dynamic ? "yes" : "no";
	buf[4] = nt->updatefreq->str;
	gtk_clist_append(GTK_CLIST(multiping_clist), buf);
	gtk_clist_set_row_data(GTK_CLIST(multiping_clist), i, nt);
    }

/* Setup */
    //  vbox = gkrellm_create_framed_tab(tabs, "Setup");

/* Info tab */
//    vbox = gkrellm_create_framed_tab(tabs, "Info");
    //    text = gkrellm_scrolled_text(vbox, NULL, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    //    for (i = 0; i < sizeof(plugin_info_text)/sizeof(gchar *); i++)
    //  gkrellm_add_info_text_string(text, plugin_info_text[i]);

/* About tab */
    info_label = gtk_label_new(plugin_about_text);
    label = gtk_label_new("About");
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), info_label, label);
}

static void apply_plugin_config()
{
    gshort row;
    gchar *s1, *s2, *s3, *s4, *s5;
    GList *new_hosts;

    if (list_modified) {
	kill_pinger();

	new_hosts = NULL;

	for (row = 0; row < GTK_CLIST(multiping_clist)->rows; row++) {
	    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 0, &s1);
	    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 1, &s2);
	    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 2, &s3);
	    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 3, &s4);
	    gtk_clist_get_text(GTK_CLIST(multiping_clist), row, 4, &s5);
	    new_hosts = append_host(new_hosts, s1, s2, strcmp(s3, "yes") == 0,strcmp(s4, "yes") == 0,s5);
	}

	g_list_foreach(hosts, (GFunc) host_free, NULL);
	g_list_free(hosts);
	gkrellm_panel_destroy(panel);
	panel = gkrellm_panel_new0();

	hosts = new_hosts;
	setup_display(1);

	list_modified = FALSE;

	launch_pipe();
    }
}

static void save_plugin_config(FILE * f)
{
    gchar *label;
    gchar *pt;
    GList *list;
    host_data *h;

    for (list = hosts; list; list = list->next) {
	h = (host_data *) list->data;

	//when saving, we convert spaces in labels to underscores
	label = g_strdup(h->name->str);
	for (pt = label; *pt; pt++)
	    if (*pt == ' ')
		*pt = '_';

	fprintf(f, "multiping host %s %s %d %s %d\n", label, h->ip->str, h->show_trip,
		h->updatefreq->str, h->dynamic);
	g_free(label);
    }
}

static void load_plugin_config(gchar * arg)
{
    gchar plugin_config[64];
    gchar item[256];
    gchar label[26];
    gchar ip[76];
    gchar updatefreq[4];
    gchar *pt;
    gshort n;
    gboolean show_trip;
    gboolean dynamic;

    n = sscanf(arg, "%s %[^\n]", plugin_config, item);

    if (n == 2) {
	if (!strcmp(plugin_config, "host")) {
	    if (delete_list) {
		g_list_foreach(hosts, (GFunc) host_free, NULL);
		g_list_free(hosts);
		delete_list = FALSE;
	    }

	    label[0] = '\0';
	    ip[0] = '\0';
	    updatefreq[0] = '\0';
	    show_trip = TRUE;
	    dynamic = FALSE;
	    sscanf(item, "%25s %75s %d %3s %d", label, ip, &show_trip, updatefreq, &dynamic);

	    //when loading, we convert underscores in labels to spaces
	    for (pt = label; *pt; pt++)
		if (*pt == '_')
		    *pt = ' ';

	    hosts = append_host(hosts, label, ip, show_trip, dynamic, updatefreq);
	}
    }
}

/* The monitor structure tells GKrellM how to call the plugin routines.
*/
static GkrellmMonitor plugin_mon = {
    CONFIG_NAME,		/* Name, for config tab.    */
    0,				/* Id,  0 if a plugin       */
    create_plugin,		/* The create function      */
    update_plugin,		/* The update function      */
    create_plugin_config,	/* The config tab create function   */
    apply_plugin_config,	/* Apply the config function        */

    save_plugin_config,		/* Save user config */
    load_plugin_config,		/* Load user config */
    "multiping",		/* config keyword                       */

    NULL,			/* Undefined 2  */
    NULL,			/* Undefined 1  */
    NULL,			/* private              */

    MON_MAIL,			/* Insert plugin before this monitor                    */

    NULL,			/* Handle if a plugin, filled in by GKrellM     */
    NULL			/* path if a plugin, filled in by GKrellM       */
};

GkrellmMonitor *gkrellm_init_plugin()
{
    list_modified = FALSE;
    delete_list = TRUE;
    style_id = gkrellm_add_meter_style(&plugin_mon, STYLE_NAME);
    monitor = &plugin_mon;
    return &plugin_mon;
}
