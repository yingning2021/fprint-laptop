/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>
#include <string.h>

#include <libfprint/fprint.h>

static FpDevice *
find_oms2388 (GPtrArray *devices)
{
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *device = g_ptr_array_index (devices, i);

      if (g_strcmp0 (fp_device_get_driver (device), "oms2388") == 0)
        return device;
    }

  return NULL;
}

static gboolean
cancel_operation (gpointer data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
  return G_SOURCE_REMOVE;
}

typedef struct
{
  GMainLoop *loop;
  FpDevice *device;
  GPtrArray *prints;
  GCancellable *cancellable;
  GError *control_error;
  gboolean identify_ok;
  gboolean matched;
} SuspendResumeTest;

static void
suspend_test_identify_done (GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
  SuspendResumeTest *test = user_data;
  g_autoptr(FpPrint) match = NULL;
  g_autoptr(FpPrint) scanned = NULL;
  g_autoptr(GError) error = NULL;

  test->identify_ok = fp_device_identify_finish (
    FP_DEVICE (source), result, &match, &scanned, &error);
  test->matched = match != NULL;
  if (!test->identify_ok && !test->control_error)
    test->control_error = g_steal_pointer (&error);
  g_main_loop_quit (test->loop);
}

static void
suspend_test_resume_done (GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
  SuspendResumeTest *test = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_resume_finish (FP_DEVICE (source), result, &error))
    {
      test->control_error = g_steal_pointer (&error);
      g_cancellable_cancel (test->cancellable);
      return;
    }
  g_print ("logical-resume-result=complete\n");
  g_print ("suspend-resume-touch-ready: touch the enrolled finger now\n");
}

static gboolean
suspend_test_resume (gpointer user_data)
{
  SuspendResumeTest *test = user_data;

  fp_device_resume (test->device, NULL, suspend_test_resume_done, test);
  return G_SOURCE_REMOVE;
}

static void
suspend_test_suspend_done (GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
  SuspendResumeTest *test = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_suspend_finish (FP_DEVICE (source), result, &error))
    {
      test->control_error = g_steal_pointer (&error);
      g_cancellable_cancel (test->cancellable);
      return;
    }
  g_print ("logical-suspend-result=complete\n");
  g_timeout_add (1000, suspend_test_resume, test);
}

static gboolean
suspend_test_suspend (gpointer user_data)
{
  SuspendResumeTest *test = user_data;

  fp_device_suspend (test->device, NULL, suspend_test_suspend_done, test);
  return G_SOURCE_REMOVE;
}

static gboolean
suspend_test_timeout (gpointer user_data)
{
  SuspendResumeTest *test = user_data;

  g_cancellable_cancel (test->cancellable);
  return G_SOURCE_REMOVE;
}

static gboolean
run_suspend_resume_test (FpDevice *device, GPtrArray *prints, GError **error)
{
  SuspendResumeTest test = {
    .loop = g_main_loop_new (NULL, FALSE),
    .device = device,
    .prints = prints,
    .cancellable = g_cancellable_new (),
  };
  guint suspend_source;
  guint timeout_source;

  g_print ("suspend-resume-test: starting identify, then pausing host polling\n");
  fp_device_identify (device, prints, test.cancellable, NULL, NULL, NULL,
                      suspend_test_identify_done, &test);
  suspend_source = g_timeout_add (300, suspend_test_suspend, &test);
  timeout_source = g_timeout_add_seconds (90, suspend_test_timeout, &test);
  g_main_loop_run (test.loop);
  if (g_main_context_find_source_by_id (NULL, suspend_source))
    g_source_remove (suspend_source);
  if (g_main_context_find_source_by_id (NULL, timeout_source))
    g_source_remove (timeout_source);
  g_main_loop_unref (test.loop);
  g_object_unref (test.cancellable);

  if (test.control_error)
    {
      g_propagate_error (error, test.control_error);
      return FALSE;
    }
  if (!test.identify_ok || !test.matched)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "identify did not match after suspend/resume");
      return FALSE;
    }
  g_print ("suspend-resume-identify-result=match\n");
  return TRUE;
}

static void
enroll_progress (FpDevice *device,
                 gint      completed_stages,
                 FpPrint  *print,
                 gpointer  user_data,
                 GError   *error)
{
  (void) print;
  (void) user_data;

  if (error)
    g_print ("enroll-stage=%d retry=%s\n", completed_stages, error->message);
  else
    g_print ("enroll-stage=%d/%d\n", completed_stages,
             fp_device_get_nr_enroll_stages (device));
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) context = fp_context_new ();
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) prints = NULL;
  GPtrArray *devices;
  FpDevice *device;
  gboolean run_identify = FALSE;
  gboolean run_verify = FALSE;
  gboolean run_enroll = FALSE;
  gboolean duplicate_test = FALSE;
  gboolean delete_enrolled_test = FALSE;
  gboolean delete_slot_one_test = FALSE;
  gboolean clear_storage_test = FALSE;
  gboolean clear_empty_test = FALSE;
  gboolean cancel_test = FALSE;
  gboolean enroll_cancel_test = FALSE;
  gboolean verify_cancel_test = FALSE;
  gboolean suspend_resume_test = FALSE;
  guint timeout_seconds = 20;

  if (argc == 2 && strcmp (argv[1], "--identify") == 0)
    run_identify = TRUE;
  else if (argc == 2 && strcmp (argv[1], "--verify") == 0)
    run_verify = TRUE;
  else if (argc == 2 && strcmp (argv[1], "--enroll") == 0)
    run_enroll = TRUE;
  else if (argc == 2 && strcmp (argv[1], "--enroll-delete-test") == 0)
    {
      run_enroll = TRUE;
      delete_enrolled_test = TRUE;
    }
  else if (argc == 2 && strcmp (argv[1], "--duplicate-test") == 0)
    {
      duplicate_test = TRUE;
    }
  else if (argc == 2 && strcmp (argv[1], "--delete-slot-one-test") == 0)
    delete_slot_one_test = TRUE;
  else if (argc == 2 && strcmp (argv[1], "--enroll-clear-test") == 0)
    {
      run_enroll = TRUE;
      clear_storage_test = TRUE;
    }
  else if (argc == 2 && strcmp (argv[1], "--clear-empty-test") == 0)
    clear_empty_test = TRUE;
  else if (argc == 2 && strcmp (argv[1], "--enroll-cancel-test") == 0)
    enroll_cancel_test = TRUE;
  else if (argc == 2 && strcmp (argv[1], "--verify-cancel-test") == 0)
    {
      run_verify = TRUE;
      verify_cancel_test = TRUE;
      timeout_seconds = 2;
    }
  else if (argc == 2 && strcmp (argv[1], "--cancel-test") == 0)
    {
      run_identify = TRUE;
      cancel_test = TRUE;
      timeout_seconds = 2;
    }
  else if (argc == 2 && strcmp (argv[1], "--suspend-resume-test") == 0)
    suspend_resume_test = TRUE;
  else if (argc != 1)
    {
      g_printerr ("usage: %s [--identify|--verify|--enroll|"
                  "--enroll-delete-test|"
                  "--duplicate-test|"
                  "--delete-slot-one-test|--enroll-clear-test|"
                  "--clear-empty-test|--enroll-cancel-test|"
                  "--verify-cancel-test|--cancel-test|"
                  "--suspend-resume-test]\n", argv[0]);
      return 1;
    }

  fp_context_enumerate (context);
  devices = fp_context_get_devices (context);
  device = find_oms2388 (devices);
  if (!device)
    {
      g_printerr ("OMS 33a7:2388 was not detected by the custom libfprint build\n");
      return 2;
    }

  g_print ("driver=%s\nname=%s\ndevice-id=%s\nfeatures=0x%x\n",
           fp_device_get_driver (device), fp_device_get_name (device),
           fp_device_get_device_id (device), fp_device_get_features (device));

  if (!fp_device_open_sync (device, NULL, &error))
    {
      g_printerr ("open failed: %s\n", error->message);
      return 3;
    }

  prints = fp_device_list_prints_sync (device, NULL, &error);
  if (!prints)
    {
      g_printerr ("list failed: %s\n", error->message);
      g_clear_error (&error);
      fp_device_close_sync (device, NULL, NULL);
      return 4;
    }

  g_print ("stored-prints=%u\n", prints->len);
  for (guint i = 0; i < prints->len; i++)
    {
      FpPrint *print = g_ptr_array_index (prints, i);
      const gchar *description = fp_print_get_description (print);
      const gchar *username = fp_print_get_username (print);

      g_print ("print[%u]=%s user=%s\n", i,
               description ? description : "(no description)",
               username ? username : "(none)");
    }

  if (suspend_resume_test)
    {
      if (prints->len == 0)
        {
          g_printerr ("suspend/resume test needs at least one stored print\n");
          fp_device_close_sync (device, NULL, NULL);
          return 26;
        }
      if (!run_suspend_resume_test (device, prints, &error))
        {
          g_printerr ("suspend/resume test failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 27;
        }
    }

  if (clear_empty_test)
    {
      if (prints->len != 0)
        {
          g_printerr ("empty clear test refused: %u templates exist\n",
                      prints->len);
          fp_device_close_sync (device, NULL, NULL);
          return 17;
        }
      if (!fp_device_clear_storage_sync (device, NULL, &error))
        {
          g_printerr ("empty clear failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 18;
        }
      g_print ("empty-clear-result=complete\n");
    }

  if (enroll_cancel_test)
    {
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      g_autoptr(FpPrint) template = g_object_ref_sink (fp_print_new (device));
      g_autoptr(FpPrint) enrolled = NULL;
      GSource *cancel_source = g_timeout_source_new_seconds (2);

      fp_print_set_finger (template, FP_FINGER_RIGHT_INDEX);
      fp_print_set_description (template, "OMS cancellation test");
      g_source_set_callback (cancel_source, cancel_operation, cancellable, NULL);
      g_source_attach (cancel_source, NULL);
      g_print ("enroll-cancel-test: waiting 2 seconds without user input\n");
      enrolled = fp_device_enroll_sync (device, template, cancellable,
                                        enroll_progress, NULL, &error);
      g_source_destroy (cancel_source);
      g_source_unref (cancel_source);
      if (enrolled ||
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_printerr ("enroll cancellation failed: %s\n",
                      error ? error->message : "operation unexpectedly completed");
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 19;
        }
      g_clear_error (&error);
      g_print ("enroll-cancel-result=clean\n");

      g_clear_pointer (&prints, g_ptr_array_unref);
      prints = fp_device_list_prints_sync (device, NULL, &error);
      if (!prints)
        {
          g_printerr ("post-cancel list failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 20;
        }
      g_print ("post-enroll-cancel-stored-prints=%u\n", prints->len);
    }

  if (duplicate_test)
    {
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      g_autoptr(FpPrint) match = NULL;
      g_autoptr(FpPrint) scanned = NULL;
      GSource *cancel_source = g_timeout_source_new_seconds (30);

      g_print ("duplicate-precheck-ready: first touch must match the "
               "already enrolled finger\n");
      g_source_set_callback (cancel_source, cancel_operation, cancellable,
                             NULL);
      g_source_attach (cancel_source, NULL);
      if (!fp_device_identify_sync (device, prints, cancellable, NULL, NULL,
                                    &match, &scanned, &error))
        {
          g_source_destroy (cancel_source);
          g_source_unref (cancel_source);
          g_printerr ("duplicate precheck failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 24;
        }
      g_source_destroy (cancel_source);
      g_source_unref (cancel_source);
      if (!match)
        {
          g_printerr ("duplicate precheck did not match; no enrollment was "
                      "started\n");
          fp_device_close_sync (device, NULL, NULL);
          return 25;
        }
      g_print ("duplicate-precheck-result=match\n");
      g_clear_pointer (&prints, g_ptr_array_unref);
      prints = fp_device_list_prints_sync (device, NULL, &error);
      if (!prints)
        {
          g_printerr ("post-duplicate list failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 21;
        }
      g_print ("duplicate-result=upper-layer-rejected\n");
      g_print ("post-duplicate-stored-prints=%u\n", prints->len);
      fp_device_close_sync (device, NULL, NULL);
      return 0;
    }

  if (run_enroll)
    {
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      g_autoptr(FpPrint) template = g_object_ref_sink (fp_print_new (device));
      g_autoptr(FpPrint) enrolled = NULL;
      GSource *cancel_source = g_timeout_source_new_seconds (90);

      fp_print_set_finger (template, FP_FINGER_RIGHT_INDEX);
      fp_print_set_username (template, g_get_user_name ());
      fp_print_set_description (template, "OMS 33a7:2388 enrollment test");
      g_source_set_callback (cancel_source, cancel_operation, cancellable, NULL);
      g_source_attach (cancel_source, NULL);
      g_print ("enroll-ready: repeatedly touch and remove the same finger "
               "(90 second timeout)\n");
      enrolled = fp_device_enroll_sync (device, template, cancellable,
                                        enroll_progress, NULL, &error);
      g_source_destroy (cancel_source);
      g_source_unref (cancel_source);
      if (!enrolled)
        {
          g_printerr ("enroll failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 8;
        }
      g_print ("enroll-result=complete\n");

      g_clear_pointer (&prints, g_ptr_array_unref);
      prints = fp_device_list_prints_sync (device, NULL, &error);
      if (!prints)
        {
          g_printerr ("post-enroll list failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 9;
        }
      g_print ("post-enroll-stored-prints=%u\n", prints->len);

      if (delete_enrolled_test)
        delete_slot_one_test = TRUE;
    }

  if (delete_slot_one_test)
    {
      FpPrint *slot_one = NULL;

      for (guint i = 0; i < prints->len; i++)
        {
          FpPrint *candidate = g_ptr_array_index (prints, i);

          if (g_strcmp0 (fp_print_get_description (candidate),
                         "OMS template 1") == 0)
            {
              slot_one = candidate;
              break;
            }
        }
      if (!slot_one)
        {
          g_printerr ("delete test refused: OMS template 1 is absent\n");
          fp_device_close_sync (device, NULL, NULL);
          return 10;
        }
      if (!fp_device_delete_print_sync (device, slot_one, NULL, &error))
        {
          g_printerr ("delete test failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 11;
        }
      g_print ("delete-result=slot-1-complete\n");

      g_clear_pointer (&prints, g_ptr_array_unref);
      prints = fp_device_list_prints_sync (device, NULL, &error);
      if (!prints)
        {
          g_printerr ("post-delete list failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 12;
        }
      g_print ("post-delete-stored-prints=%u\n", prints->len);
    }

  if (clear_storage_test)
    {
      if (prints->len < 2)
        {
          g_printerr ("clear test refused: expected at least two templates, "
                      "found %u\n", prints->len);
          fp_device_close_sync (device, NULL, NULL);
          return 13;
        }
      g_print ("clear-storage-start: deleting all %u templates\n", prints->len);
      if (!fp_device_clear_storage_sync (device, NULL, &error))
        {
          g_printerr ("clear storage failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 14;
        }
      g_print ("clear-storage-result=complete\n");

      g_clear_pointer (&prints, g_ptr_array_unref);
      prints = fp_device_list_prints_sync (device, NULL, &error);
      if (!prints)
        {
          g_printerr ("post-clear list failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 15;
        }
      g_print ("post-clear-stored-prints=%u\n", prints->len);
      if (prints->len != 0)
        {
          g_printerr ("clear storage incomplete: %u templates remain\n",
                      prints->len);
          fp_device_close_sync (device, NULL, NULL);
          return 16;
        }
    }

  if (run_verify)
    {
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      g_autoptr(FpPrint) scanned = NULL;
      GSource *cancel_source;
      gboolean matched = FALSE;

      if (prints->len == 0)
        {
          g_printerr ("verify needs at least one stored print\n");
          fp_device_close_sync (device, NULL, NULL);
          return 21;
        }
      if (verify_cancel_test)
        g_print ("verify-cancel-test: waiting %u seconds without user input\n",
                 timeout_seconds);
      else
        g_print ("verify-ready: touch a finger (%u second timeout)\n",
                 timeout_seconds);
      cancel_source = g_timeout_source_new_seconds (timeout_seconds);
      g_source_set_callback (cancel_source, cancel_operation, cancellable, NULL);
      g_source_attach (cancel_source, NULL);
      if (!fp_device_verify_sync (device, g_ptr_array_index (prints, 0),
                                  cancellable, NULL, NULL, &matched,
                                  &scanned, &error))
        {
          g_source_destroy (cancel_source);
          g_source_unref (cancel_source);
          if (verify_cancel_test &&
              g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              g_print ("verify-cancel-result=clean\n");
              g_clear_error (&error);
              goto close_device;
            }
          g_printerr ("verify failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 22;
        }
      g_source_destroy (cancel_source);
      g_source_unref (cancel_source);
      g_print ("verify-result=%s\n", matched ? "match" : "no-match");
    }

  if (run_identify)
    {
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      g_autoptr(FpPrint) match = NULL;
      g_autoptr(FpPrint) scanned = NULL;
      GSource *cancel_source;

      if (prints->len == 0)
        {
          g_printerr ("identify needs at least one stored print\n");
          fp_device_close_sync (device, NULL, NULL);
          return 6;
        }

      if (cancel_test)
        g_print ("cancel-test: waiting %u seconds without user input\n",
                 timeout_seconds);
      else
        g_print ("identify-ready: touch an enrolled finger (%u second timeout)\n",
                 timeout_seconds);
      cancel_source = g_timeout_source_new_seconds (timeout_seconds);
      g_source_set_callback (cancel_source, cancel_operation, cancellable, NULL);
      g_source_attach (cancel_source, NULL);
      if (!fp_device_identify_sync (device, prints, cancellable, NULL, NULL,
                                    &match, &scanned, &error))
        {
          g_source_destroy (cancel_source);
          g_source_unref (cancel_source);
          if (cancel_test &&
              g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              g_print ("cancel-result=clean\n");
              g_clear_error (&error);
              goto close_device;
            }
          g_printerr ("identify failed: %s\n", error->message);
          g_clear_error (&error);
          fp_device_close_sync (device, NULL, NULL);
          return 7;
        }
      g_source_destroy (cancel_source);
      g_source_unref (cancel_source);
      g_print ("identify-result=%s\n", match ? "match" : "no-match");
    }

close_device:
  if (!fp_device_close_sync (device, NULL, &error))
    {
      g_printerr ("close failed: %s\n", error->message);
      return 5;
    }

  return 0;
}
