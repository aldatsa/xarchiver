/*
 *  Copyright (C) 2008 Giuseppe Torelli - <colossus73@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 */

#include <string.h>
#include "tar.h"
#include "gzip_et_al.h"
#include "interface.h"
#include "main.h"
#include "string_utils.h"
#include "support.h"
#include "window.h"

#define TMPFILE "xa-tmp.tar"

static void xa_add_delete_bzip2_gzip_lzma_compressed_tar(GString *, XArchive *, gboolean);
static gboolean xa_concat_filenames (GtkTreeModel *model,GtkTreePath *path,GtkTreeIter *iter,GSList **list);
static gboolean xa_extract_tar_without_directories(gchar *, XArchive *, gchar *);

gboolean isTar (FILE *file)
{
	unsigned char magic[7];

	fseek(file, 0, SEEK_SET);

	if (fseek(file, 257, SEEK_CUR) != 0 ||
	    fread(magic, 1, sizeof(magic), file) != sizeof(magic))
	{
		fseek(file, 0, SEEK_SET);
		return FALSE;
	}

	fseek(file, 0, SEEK_SET);

	return (memcmp(magic, "\x75\x73\x74\x61\x72\x00\x30", sizeof(magic)) == 0 ||
	        memcmp(magic, "\x75\x73\x74\x61\x72\x20\x20", sizeof(magic)) == 0 ||
	        memcmp(magic, "\x0\x0\x0\x0\x0\x0\x0", sizeof(magic)) == 0);
}

void xa_tar_ask (XArchive *archive)
{
	archive->can_add = archive->can_extract = archive->can_test = TRUE;
	archive->can_delete = TRUE;
	archive->can_touch = TRUE;
	archive->can_move = TRUE;
	archive->can_overwrite = TRUE;
	archive->can_full_path = TRUE;
	archive->can_update = TRUE;
	archive->can_recurse = TRUE;
}

void xa_tar_open (XArchive *archive)
{
	gchar *command;
	guint i;

	command = g_strconcat (tar, " tfv " , archive->path[1], NULL);
	archive->files_size = 0;
	archive->nr_of_files = 0;
	archive->nc = 7;
	archive->parse_output = xa_tar_parse_output;
	xa_spawn_async_process (archive,command);

	g_free (command);

	GType types[]= {GDK_TYPE_PIXBUF,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_UINT64,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_POINTER};
	archive->column_types = g_malloc0(sizeof(types));
	for (i = 0; i < 9; i++)
		archive->column_types[i] = types[i];

	char *names[]= {(_("Points to")),(_("Permissions")),(_("Owner/Group")),(_("Size")),(_("Date")),(_("Time")),NULL};
	xa_create_liststore (archive,names);
}

void xa_tar_parse_output (gchar *line, XArchive *archive)
{
	gchar *filename;
	gpointer item[6];
	gint n = 0, a = 0 ,linesize = 0;

	linesize = strlen(line);
	archive->nr_of_files++;

	/* Permissions */
	line[10] = '\0';
	item[1] = line;

	/* Owner */
	for(n=13; n < linesize; ++n)
		if(line[n] == ' ')
			break;
	line[n] = '\0';
	item[2] = line+11;

	/* Size */
	for(++n; n < linesize; ++n)
		if(line[n] >= '0' && line[n] <= '9')
			break;
	a = n;

	for(; n < linesize; ++n)
		if(line[n] == ' ')
			break;

	line[n] = '\0';
	item[3] = line + a;
	archive->files_size += g_ascii_strtoull(item[3],NULL,0);
	a = ++n;

	/* Date */
	for(; n < linesize; n++)
		if(line[n] == ' ')
			break;

	line[n] = '\0';
	item[4] = line + a;

	/* Time */
	a = ++n;
	for (; n < linesize; n++)
		if (line[n] == ' ')
			break;

	line[n] = '\0';
	item[5] = line + a;
	n++;
	line[linesize-1] = '\0';

	filename = line + n;

	/* Symbolic link */
	gchar *temp = g_strrstr (filename,"->");
	if (temp)
	{
		gint len = strlen(filename) - strlen(temp);
		item[0] = (filename +=3) + len;
		filename[strlen(filename) - strlen(temp)-1] = '\0';
	}
	else
		item[0] = NULL;

	if(line[0] == 'd')
	{
		/* Work around for gtar, which does not output / with directories */
		if(line[linesize-2] != '/')
			filename = g_strconcat(line + n, "/", NULL);
		else
			filename = g_strdup(line + n);
	}
	else
		filename = g_strdup(line + n);
	xa_set_archive_entries_for_each_row (archive,filename,item);
	g_free(filename);
}

void xa_tar_delete (XArchive *archive, GSList *file_list)
{
	GString *files;
	gchar *command;
	GSList *list = NULL;

	files = xa_quote_filenames(file_list, NULL, TRUE);

	if (is_tar_compressed(archive->type))
		xa_add_delete_bzip2_gzip_lzma_compressed_tar(files,archive,0);
	else
	{
		command = g_strconcat(tar, " --delete -vf ", archive->path[1], files->str, NULL);
		list = g_slist_append(list,command);
		xa_run_command (archive,list);
	}
}

void xa_tar_add (XArchive *archive, GSList *file_list, gchar *compression)
{
	GString *files;
	GSList *list = NULL;
	gchar *command = NULL;

	if (archive->location_entry_path != NULL)
		archive->working_dir = g_strdup(archive->tmp);

	files = xa_quote_filenames(file_list, NULL, TRUE);

	switch (archive->type)
	{
		case XARCHIVETYPE_TAR:
		if ( g_file_test (archive->path[1],G_FILE_TEST_EXISTS))
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									archive->do_update ? "-uvvf " : "-rvvf ",
									archive->path[1],
									files->str , NULL );
		else
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									"-cvvf ",archive->path[1],
									files->str , NULL );
		break;

		case XARCHIVETYPE_TAR_BZ2:
		if ( g_file_test ( archive->path[1] , G_FILE_TEST_EXISTS ) )
			xa_add_delete_bzip2_gzip_lzma_compressed_tar (files,archive,1);
		else
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									"-cvvjf ",archive->path[1],
									files->str , NULL );
		break;

		case XARCHIVETYPE_TAR_GZ:
		if ( g_file_test ( archive->path[1] , G_FILE_TEST_EXISTS ) )
			xa_add_delete_bzip2_gzip_lzma_compressed_tar (files,archive,1);
		else
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									"-cvvzf ",archive->path[1],
									files->str , NULL );
		break;

		case XARCHIVETYPE_TAR_LZMA:
		if ( g_file_test ( archive->path[1] , G_FILE_TEST_EXISTS ) )
			xa_add_delete_bzip2_gzip_lzma_compressed_tar (files,archive,1);
		else
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									"--use-compress-program=lzma -cvvf ",archive->path[1],
									files->str , NULL );
		break;

		case XARCHIVETYPE_TAR_XZ:
		if ( g_file_test ( archive->path[1] , G_FILE_TEST_EXISTS ) )
			xa_add_delete_bzip2_gzip_lzma_compressed_tar (files,archive,1);
		else
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									"--use-compress-program=xz -cvvf ",archive->path[1],
									files->str , NULL );
		break;

		case XARCHIVETYPE_TAR_LZOP:
		if ( g_file_test ( archive->path[1] , G_FILE_TEST_EXISTS ) )
			xa_add_delete_bzip2_gzip_lzma_compressed_tar (files,archive,1);
		else
			command = g_strconcat (tar, " ",
									archive->do_recurse ? "" : "--no-recursion ",
									archive->do_move ? "--remove-files " : "",
									"--use-compress-program=lzop -cvvf ",archive->path[1],
									files->str , NULL );
		break;

		case XARCHIVETYPE_BZIP2:
			command = g_strconcat("sh -c \"bzip2 -c ",files->str,"> ",archive->path[1],"\"",NULL);
		break;

		case XARCHIVETYPE_GZIP:
			command = g_strconcat("sh -c \"gzip -c ",files->str,"> ",archive->path[1],"\"",NULL);
		break;

		case XARCHIVETYPE_LZMA:
			command = g_strconcat("sh -c \"lzma -c ",files->str,"> ",archive->path[1],"\"",NULL);
		break;

		case XARCHIVETYPE_LZOP:
			command = g_strconcat("sh -c \"lzop -c ",files->str,"> ",archive->path[1],"\"",NULL);
		break;

		case XARCHIVETYPE_XZ:
			if (!compression)
				compression = "5";
			command = g_strconcat("sh -c \"xz", " -", compression, " -c ", files->str, "> ", archive->path[1], "\"", NULL);
		break;

		default:
		command = NULL;
	}

	if (command != NULL)
	{
		g_string_free(files,TRUE);
		list = g_slist_append(list,command);
		xa_run_command (archive,list);
	}
}

/*
 * Note: tar lists '\' as '\\' while it extracts '\', i.e.
 * file names containing this character can't be handled entirely.
 */

gboolean xa_tar_extract (XArchive *archive, GSList *file_list)
{
	GString *files;
	gchar *command;
	GSList *list = NULL;
	gboolean result = FALSE;

	files = xa_quote_filenames(file_list, NULL, TRUE);

	switch (archive->type)
	{
		case XARCHIVETYPE_TAR:
		if (archive->do_full_path || opt_multi_extract)
		{
			command = g_strconcat (tar, " -xvf " , archive->path[1],
						#ifdef __FreeBSD__
								archive->do_overwrite ? " " : " -k",
						#else
								archive->do_overwrite ? " --overwrite" : " --keep-old-files",
						#endif
								archive->do_touch ? " --touch" : "",
								" -C ", archive->extraction_path, files->str, NULL);
		}
		else
		{
			result = xa_extract_tar_without_directories ( "tar -xvf ",archive,files->str);
			command = NULL;
		}
		break;

		case XARCHIVETYPE_TAR_BZ2:
		if (archive->do_full_path || opt_multi_extract)
		{
			command = g_strconcat (tar, " -xjvf " , archive->path[1],
						#ifdef __FreeBSD__
								archive->do_overwrite ? " " : " -k",
						#else
								archive->do_overwrite ? " --overwrite" : " --keep-old-files",
						#endif
								archive->do_touch ? " --touch" : "",
								" -C ", archive->extraction_path, files->str, NULL);
		}
		else
		{
			result = xa_extract_tar_without_directories ( "tar -xjvf ",archive,files->str);
			command = NULL;
		}
		break;

		case XARCHIVETYPE_TAR_GZ:
		if (archive->do_full_path || opt_multi_extract)
		{
			command = g_strconcat (tar, " -xzvf " , archive->path[1],
						#ifdef __FreeBSD__
								archive->do_overwrite ? " " : " -k",
						#else
								archive->do_overwrite ? " --overwrite" : " --keep-old-files",
						#endif
								archive->do_touch ? " --touch" : "",
								" -C ", archive->extraction_path, files->str, NULL);
		}
		else
		{
			result = xa_extract_tar_without_directories ( "tar -xzvf ",archive,files->str);
			command = NULL;
		}
		break;

		case XARCHIVETYPE_TAR_LZMA:
		if (archive->do_full_path || opt_multi_extract)
		{
			command = g_strconcat (tar, " --use-compress-program=lzma -xvf " , archive->path[1],
						#ifdef __FreeBSD__
								archive->do_overwrite ? " " : " -k",
						#else
								archive->do_overwrite ? " --overwrite" : " --keep-old-files",
						#endif
								archive->do_touch ? " --touch" : "",
								" -C ", archive->extraction_path, files->str, NULL);
		}
		else
		{
			result = xa_extract_tar_without_directories ( "tar --use-compress-program=lzma -xvf ",archive,files->str);
			command = NULL;
		}
		break;

		case XARCHIVETYPE_TAR_LZOP:
		if (archive->do_full_path || opt_multi_extract)
		{
			command = g_strconcat (tar, " --use-compress-program=lzop -xvf " , archive->path[1],
						#ifdef __FreeBSD__
								archive->do_overwrite ? " " : " -k",
						#else
								archive->do_overwrite ? " --overwrite" : " --keep-old-files",
						#endif
								archive->do_touch ? " --touch" : "",
								" -C ", archive->extraction_path, files->str, NULL);
		}
		else
		{
			result = xa_extract_tar_without_directories ( "tar --use-compress-program=lzop -xvf ",archive,files->str);
			command = NULL;
		}
		break;

		case XARCHIVETYPE_TAR_XZ:
		if (archive->do_full_path || opt_multi_extract)
		{
			command = g_strconcat (tar, " --use-compress-program=xz -xvf " , archive->path[1],
						#ifdef __FreeBSD__
								archive->do_overwrite ? " " : " -k",
						#else
								archive->do_overwrite ? " --overwrite" : " --keep-old-files",
						#endif
								archive->do_touch ? " --touch" : "",
								" -C ", archive->extraction_path, files->str, NULL);
		}
		else
		{
			result = xa_extract_tar_without_directories ( "tar --use-compress-program=xz -xvf ",archive,files->str);
			command = NULL;
		}
		break;

		case XARCHIVETYPE_LZMA:
		case XARCHIVETYPE_LZOP:
		case XARCHIVETYPE_BZIP2:
		case XARCHIVETYPE_XZ:
		result = xa_gzip_et_al_extract(archive,NULL);
		command = NULL;
		break;

		case XARCHIVETYPE_GZIP:
		result = xa_gzip_et_al_extract(archive,NULL);
		command = NULL;
		break;

		default:
		command = NULL;
	}

	if (command != NULL)
	{
		list = g_slist_append(list,command);
		result = xa_run_command (archive,list);
	}

	g_string_free(files, TRUE);

	return result;
}

static void xa_add_delete_bzip2_gzip_lzma_compressed_tar (GString *files, XArchive *archive, gboolean add)
{
	gchar *command,*executable = NULL,*filename = NULL;
	gboolean result;
	GSList *list = NULL;

	switch (archive->type)
	{
		case XARCHIVETYPE_TAR_BZ2:
			executable = "bzip2 -f ";
			filename = TMPFILE ".bz2";
		break;
		case XARCHIVETYPE_TAR_GZ:
			executable = "gzip -f ";
			filename = TMPFILE ".gz";
		break;
		case XARCHIVETYPE_TAR_LZMA:
			executable = "lzma -f ";
			filename = TMPFILE ".lzma";
		break;
		case XARCHIVETYPE_TAR_XZ:
			executable = "xz -f ";
			filename = TMPFILE ".xz";
		break;
		case XARCHIVETYPE_TAR_LZOP:
			executable = "lzop -f ";
			filename = TMPFILE ".lzo";
		break;

		default:
		break;
	}
	/* Let's copy the archive to /tmp first */
	result = xa_create_temp_directory(archive);
	if (!result)
		return;

	/* Let's copy the archive to /tmp first */
	command = g_strconcat ("cp -a ",archive->path[1]," ",archive->tmp,"/",filename,NULL);
	list = g_slist_append(list,command);

	command = g_strconcat (executable,"-d ",archive->tmp,"/",filename,NULL);
	list = g_slist_append(list,command);

	if (add)
		command = g_strconcat (tar, " ",
							archive->do_recurse ? "" : "--no-recursion ",
							archive->do_move ? "--remove-files " : "",
							archive->do_update ? "-uvvf " : "-rvvf ",
							archive->tmp,"/" TMPFILE,
							files->str , NULL );
	else
		command = g_strconcat(tar, " --no-wildcards --delete -f ", archive->tmp, "/" TMPFILE, files->str, NULL);
	list = g_slist_append(list,command);

	command = g_strconcat (executable,archive->tmp,"/" TMPFILE,NULL);
	list = g_slist_append(list,command);

	/* Let's move the modified archive from /tmp to the original archive location */
	command = g_strconcat ("mv ",archive->tmp,"/",filename," ",archive->path[1],NULL);
	list = g_slist_append(list,command);
	xa_run_command (archive,list);
}

gboolean is_tar_compressed (gint type)
{
	return (type == XARCHIVETYPE_TAR_BZ2 || type == XARCHIVETYPE_TAR_GZ || type == XARCHIVETYPE_TAR_LZMA || type == XARCHIVETYPE_TAR_LZOP || type == XARCHIVETYPE_TAR_XZ);
}

static gboolean xa_extract_tar_without_directories (gchar *string, XArchive *archive, gchar *files_to_extract)
{
	GString *files = NULL;
	gchar *command;
	GSList *list = NULL;
	GSList *file_list = NULL;
	gboolean result;

	result = xa_create_temp_directory (archive);
	if (!result)
		return FALSE;

	if (strlen(files_to_extract) == 0)
	{
		gtk_tree_model_foreach(GTK_TREE_MODEL(archive->liststore),(GtkTreeModelForeachFunc) xa_concat_filenames,&file_list);
		files = xa_quote_filenames(file_list, NULL, TRUE);
		files_to_extract = files->str;
	}

	command = g_strconcat (string, archive->path[1],
										#ifdef __FreeBSD__
											archive->do_overwrite ? " " : " -k",
										#else
											archive->do_overwrite ? " --overwrite" : " --keep-old-files",
											" --no-wildcards ",
										#endif
										archive->do_touch ? " --touch" : "",
										" -C ", archive->tmp, files_to_extract, NULL);
	list = g_slist_append(list,command);
	if (strstr(files_to_extract,"/") || strcmp(archive->tmp,archive->extraction_path) != 0)
	{
		archive->working_dir = g_strdup(archive->tmp);
		command = g_strconcat ("mv -f ",files_to_extract," ",archive->extraction_path,NULL);
		list = g_slist_append(list,command);
	}

	if (files)
		g_string_free(files, TRUE);

	return xa_run_command (archive,list);
}

static gboolean xa_concat_filenames (GtkTreeModel *model,GtkTreePath *path,GtkTreeIter *iter,GSList **list)
{
	XEntry *entry;
	gint current_page,idx;

	current_page = gtk_notebook_get_current_page(notebook);
	idx = xa_find_archive_index (current_page);

	gtk_tree_model_get(model,iter,archive[idx]->nc+1,&entry,-1);
	if (entry == NULL)
		return TRUE;
	else
		xa_fill_list_with_recursed_entries(entry->child,list);
	return FALSE;
}

void xa_tar_test(XArchive *archive)
{
	gchar *command;
	GSList *list = NULL;
	GString *names = g_string_new("");

	switch (archive->type)
	{
		case XARCHIVETYPE_TAR:
			command = g_strconcat (tar, " -tvf ",archive->path[0], NULL);
		break;

		case XARCHIVETYPE_TAR_BZ2:
			command = g_strconcat (tar, " -tjvf ",archive->path[0], NULL);
		break;

		case XARCHIVETYPE_TAR_GZ:
			command = g_strconcat (tar, " -tzvf ",archive->path[0], NULL);
		break;

		case XARCHIVETYPE_TAR_LZMA:
			command = g_strconcat (tar, " --use-compress-program=lzma -tvf ",archive->path[0], NULL);
		break;

		case XARCHIVETYPE_TAR_LZOP:
			command = g_strconcat (tar, " --use-compress-program=lzop -tvf ",archive->path[0], NULL);
		break;

		case XARCHIVETYPE_TAR_XZ:
			command = g_strconcat (tar, " --use-compress-program=xz -tvf ",archive->path[0], NULL);
		break;

		case XARCHIVETYPE_LZMA:
		case XARCHIVETYPE_LZOP:
		case XARCHIVETYPE_BZIP2:
		case XARCHIVETYPE_GZIP:
		case XARCHIVETYPE_XZ:
			xa_gzip_et_al_test(archive);
			command = NULL;
		break;

		default:
			command = NULL;
	}
	if (command != NULL)
	{
		g_string_free(names,TRUE);
		list = g_slist_append(list,command);
		xa_run_command (archive,list);
	}
}
