#include <gtk/gtk.h>
#include <stdio.h>
#include <stdarg.h>
#include "blursk.h"
#define TITLEFONT "-*-helvetica-bold-r-normal--*-180-*-*-*-*-*-*"
#define TEXTFONT "-*-courier-medium-r-normal--*-120-*-*-*-*-*-*"

typedef struct
{
	gboolean  open;
	GtkWidget *window;
	GtkWidget *close;
	GtkWidget *text;
} dialog_t;
static dialog_t about_dialog;
static dialog_t error_dialog;

static void close_cb(GtkWidget *w, gpointer data)
{
	if (about_dialog.open &&
		(w == about_dialog.window || w == about_dialog.close))
	{
		gtk_widget_destroy(about_dialog.window);
		about_dialog.open = FALSE;
	}
	else if (error_dialog.open &&
		(w == error_dialog.window || w == error_dialog.close))
	{
		gtk_widget_destroy(error_dialog.window);
		error_dialog.open = FALSE;
	}
}

static void addtext(GtkWidget *twidget, const char *text)
{
	GdkFont *font;
	const char	*body;
	int	len;

	/* find the end of first line */
	for (len = 0, body = text; *body && *body != '\n'; body++, len++)
	{
	}
	if (*body == '\n');
		body++, len++;

	/* Add the first line in the "TITLEFONT" */
	font = gdk_font_load(TITLEFONT);
	gtk_text_insert(GTK_TEXT(twidget), font, NULL, NULL, text, len);
	gdk_font_unref(font);

	/* If there's any remaining text, add it in "TEXTFONT" */
	if (*body)
	{
		font = gdk_font_load(TEXTFONT);
		gtk_text_insert(GTK_TEXT(twidget), font, NULL, NULL, body, -1);
		gdk_font_unref(font);
	}
}

static dialog_t showtext(const char *text, const char *close_label)
{
	GtkWidget *vbox, *scroll, *bbox, *btn;
	dialog_t ret;

	/* Create a new window */
	ret.window = gtk_window_new(GTK_WINDOW_DIALOG);
	gtk_container_border_width(GTK_CONTAINER(ret.window), 10);
	gtk_window_position(GTK_WINDOW(ret.window), GTK_WIN_POS_MOUSE);

	/* Create a vbox inside the window */
	vbox = gtk_vbox_new(FALSE, 10);
	gtk_container_add(GTK_CONTAINER(ret.window), vbox);

	/* Create a scrolled window in the vbox */
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
	gtk_box_pack_start(GTK_BOX(vbox), scroll, FALSE, FALSE, 0);

	/* Create a text widget in the scrolled window */
	ret.text = gtk_text_new(NULL, NULL);
	gtk_widget_set_usize(ret.text, 450, 300);
	gtk_container_add(GTK_CONTAINER(scroll), ret.text);

	/* Stuff the text into it, using the Courier font */
	addtext(ret.text, text);
	gtk_text_set_editable(GTK_TEXT(ret.text), FALSE);

	/* Create a button box under the text widget */
	bbox = gtk_hbutton_box_new();
	gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 5);
	gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);

	/* Add a [Close] button */
	ret.close = gtk_button_new_with_label(close_label);
	gtk_signal_connect_object(GTK_OBJECT(ret.close), "clicked", GTK_SIGNAL_FUNC(close_cb), GTK_OBJECT(ret.window));
	GTK_WIDGET_SET_FLAGS(ret.close, GTK_CAN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(bbox), ret.close, TRUE, TRUE, 0);
	gtk_widget_grab_default(ret.close);

	/* Also make the window manager's "close" button close the window */
	gtk_signal_connect_object(GTK_OBJECT(ret.window), "delete_event", GTK_SIGNAL_FUNC(close_cb), GTK_OBJECT(ret.window));

	/* Show everything */
	gtk_widget_show(ret.close);
	gtk_widget_show(bbox);
	gtk_widget_show(ret.text);
	gtk_widget_show(scroll);
	gtk_widget_show(vbox);
	gtk_widget_show(ret.window);

	ret.open = TRUE;
	return ret;
}

void about()
{
	/* If already showing the window, do nothing */
	if (about_dialog.open)
		return;

	about_dialog = showtext(readme, "Close");
}

void about_error(char *format, ...)
{
	char	buf[2000];
	va_list	args;

	/* format the error message */
	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	/* if no error window yet, then create one */
	if (error_dialog.open)
		addtext(error_dialog.text, buf);
	else
		error_dialog = showtext(buf, "Shit Happens");
}

/*main(int argc, char **argv)
{
	gtk_init(&argc, &argv);
	about();
	gtk_main();
	return 0;
}*/
