$(document).ready(function(){

  var RESTORE_SETTINGS_UPLOAD_ID = 'restore-settings-button';
  var RESTORE_SETTINGS_FILE_ID = 'restore-settings-file';
  var UPLOAD_CONTENT_ALLOWED_DIV_ID = 'upload-content-allowed';
  var UPLOAD_CONTENT_RECOVERING_DIV_ID = 'upload-content-recovering';
  var INSTALLED_CONTENT_FILENAME_ID = 'installed-content-filename';
  var INSTALLED_CONTENT_NAME_ID = 'installed-content-name';
  var INSTALLED_CONTENT_CREATED_ID = 'installed-content-created';
  var INSTALLED_CONTENT_INSTALLED_ID = 'installed-content-installed';
  var INSTALLED_CONTENT_INSTALLED_BY_ID = 'installed-content-installed-by';

  var isRestoring = false;
  var restoreErrorShown = false;

  function progressBarHTML(extraClass, label) {
    var html = "<div class='progress'>";
    html += "<div class='" + extraClass + " progress-bar progress-bar-success progress-bar-striped active' role='progressbar' aria-valuemin='0' aria-valuemax='100'>";
    html += "<span class='ongoing-msg'></span></div></div>";
    return html;
  }

  function showUploadProgress(title) {
    swal({
      title: title,
      text: progressBarHTML('upload-content-progress', 'Upload'),
      html: true,
      showConfirmButton: false,
      allowEscapeKey: false
    });
  }

  function uploadNextChunk(file, offset, id) {
      if (offset == undefined) {
          offset = 0;
      }
      if (id == undefined) {
          // Identify this upload session
          id = Math.round(Math.random() * 2147483647);
      }

      var fileSize = file.size;
      var filename = file.name;

      var CHUNK_SIZE = 1048576; // 1 MiB

      var isFinal = Boolean(fileSize - offset <= CHUNK_SIZE);
      var nextChunkSize = Math.min(fileSize - offset, CHUNK_SIZE);
      var chunk = file.slice(offset, offset + nextChunkSize, file.type);
      var chunkFormData = new FormData();

      var formItemName = 'restore-file-chunk';
      if (offset == 0) {
          formItemName = isFinal ? 'restore-file-chunk-only' : 'restore-file-chunk-initial';
      } else if (isFinal) {
          formItemName = 'restore-file-chunk-final';
      }

      chunkFormData.append(formItemName, chunk, filename);
      var ajaxParams = {
        url: '/content/upload',
        type: 'POST',
        timeout: 30000, // 30 s
        headers: {"X-Session-Id": id},
        cache: false,
        processData: false,
        contentType: false,
        data: chunkFormData
      };

      var ajaxObject = $.ajax(ajaxParams);
      ajaxObject.fail(function (jqXHR, textStatus, errorThrown) {
        // status of 0 means the connection was reset, which
        // happens after the content is parsed and the server restarts
        // in the case of json and json.gz files
        if (jqXHR.status != 0) {
          showErrorMessage(
            "Error",
            "The content couldn't be put in your world.\n"
            + "Check that the file is a backup (.zip) or a file of objects (.json or .json.gz), and try again."
          );
        } else {
            isRestoring = true;

            // immediately reload backup information since one should be restoring now
            reloadBackupInformation();

            swal.close();
        }
      });

      updateProgressBars($('.upload-content-progress'), (offset + nextChunkSize) * 100 / fileSize);

      if (!isFinal) {
        ajaxObject.done(function (data, textStatus, jqXHR)
          { uploadNextChunk(file, offset + CHUNK_SIZE, id); });
      } else {
        ajaxObject.done(function(data, textStatus, jqXHR) {
          isRestoring = true;

          // immediately reload backup information since one should be restoring now
          reloadBackupInformation();

          swal.close();
        });
      }
      
    }
  
  function setupBackupUpload() {
    // construct the HTML needed for the settings backup panel
    var html = "<div class='form-group'><div id='" + UPLOAD_CONTENT_ALLOWED_DIV_ID + "'>";

    html += "<span class='help-block'>Choose a backup (.zip) or a file of objects (.json or .json.gz). Everything in your world "
      + "now is replaced by what's in the file. Your backups stay as they are, so you can go back to one of them.</span>";

    html += "<div class='dr-file-pick'>";
    html += "<label class='btn btn-default' for='restore-settings-file'><span class='glyphicon glyphicon-folder-open'></span> Choose a file</label>";
    html += "<input id='restore-settings-file' name='restore-settings' type='file' accept='.zip,.json,.gz'>";
    html += "<span class='dr-file-name' id='restore-settings-file-name'>No file chosen</span>";
    html += "<button type='button' id='" + RESTORE_SETTINGS_UPLOAD_ID + "' disabled='true' class='btn btn-primary'>"
      + "<span class='glyphicon glyphicon-upload'></span> Replace the content</button>";
    html += "</div>";

    html += "</div><div id='" + UPLOAD_CONTENT_RECOVERING_DIV_ID + "'>";
    html += "<span class='help-block'>Putting the content in your world&hellip;</span>";
    html += progressBarHTML('recovery', 'Restoring');
    html += "</div></div>";
    $('#' + Settings.UPLOAD_CONTENT_BACKUP_PANEL_ID + ' .panel-body').html(html);
  }

  function setupInstalledContentInfo() {
    var html = "<p class='dr-empty' id='installed-content-none'>Nothing has been put in from a file yet: your world has "
      + "what's been built in it.</p>";
    html += "<dl class='dr-facts' id='installed-content-facts' style='display: none;'>";
    html += "<div><dt>Name</dt><dd id='" + INSTALLED_CONTENT_NAME_ID + "'></dd></div>";
    html += "<div><dt>File</dt><dd id='" + INSTALLED_CONTENT_FILENAME_ID + "'></dd></div>";
    html += "<div><dt>Made</dt><dd id='" + INSTALLED_CONTENT_CREATED_ID + "'></dd></div>";
    html += "<div><dt>Put in</dt><dd id='" + INSTALLED_CONTENT_INSTALLED_ID + "'></dd></div>";
    html += "<div><dt>By</dt><dd id='" + INSTALLED_CONTENT_INSTALLED_BY_ID + "'></dd></div>";
    html += "</dl>";
    $('#' + Settings.INSTALLED_CONTENT + ' .panel-body').html(html);
  }

  // handle content archive or entity file upload

  // when the selected file is changed, enable the button if there's a selected file
  $('body').on('change', '#' + RESTORE_SETTINGS_FILE_ID, function() {
    $('#' + RESTORE_SETTINGS_UPLOAD_ID).attr('disabled', $(this).val().length == 0);
    var files = $(this).prop('files');
    $('#restore-settings-file-name').text(files && files.length ? files[0].name : "No file chosen");
  });

  // when the upload button is clicked, send the file to the DS
  // and reload the page if restore was successful or
  // show an error if not
  $('body').on('click', '#' + RESTORE_SETTINGS_UPLOAD_ID, function(e){
    e.preventDefault();

    swalAreYouSure(
      "Everything in your world now is replaced by what's in the file. Visitors are disconnected for a moment while the world restarts.",
      "Replace the content",
      function() {
        var files = $('#' + RESTORE_SETTINGS_FILE_ID).prop('files');
        var file = files[0];

        showUploadProgress("Sending " + file.name);
        uploadNextChunk(file);
      }
    );
  });

  var GENERATE_ARCHIVE_BUTTON_ID = 'generate-archive-button';
  var CONTENT_ARCHIVES_NORMAL_ID = 'content-archives-success';
  var CONTENT_ARCHIVES_CONTENT_INFO_ID = 'content-archives-content-info';
  var CONTENT_ARCHIVES_ERROR_ID = 'content-archives-error';
  var AUTOMATIC_ARCHIVES_TABLE_ID = 'automatic-archives-table';
  var AUTOMATIC_ARCHIVES_TBODY_ID = 'automatic-archives-tbody';
  var MANUAL_ARCHIVES_TABLE_ID = 'manual-archives-table';
  var MANUAL_ARCHIVES_TBODY_ID = 'manual-archives-tbody';
  var AUTO_ARCHIVES_SETTINGS_LINK_ID = 'auto-archives-settings-link';
  var ACTION_MENU_CLASS = 'action-menu';

  var automaticBackups = [];
  var manualBackups = [];

  function setupContentArchives() {
    // construct the HTML needed for the content archives panel
    var html = "<div id='" + CONTENT_ARCHIVES_NORMAL_ID + "'><div class='form-group'>";
    html += "<label class='control-label'>Made automatically</label>";
    html += "<span class='help-block'>Your server backs up your world's content and settings on a schedule, keeping the latest of "
      + "each kind. Use the menu on a backup to go back to it, or to download it. ";
    html += "<a href='/settings/#automatic_content_archives' id='" + AUTO_ARCHIVES_SETTINGS_LINK_ID + "'>Change how often backups are made</a></span>";
    html += "</div>";
    html += "<table class='table sortable' id='" + AUTOMATIC_ARCHIVES_TABLE_ID + "'>";

    var backups_table_head = "<thead><tr class='gray-tr'><th>Backup</th><th data-defaultsort='desc'>Made</th>"
      + "<th data-defaultsort='disabled'></th><th class='" + ACTION_MENU_CLASS + "' data-defaultsort='disabled'></th>"
      + "</tr></thead>";

    html += backups_table_head;
    html += "<tbody id='" + AUTOMATIC_ARCHIVES_TBODY_ID + "'></tbody></table>";
    html += "<div class='form-group'>";
    html += "<label class='control-label'>Made by you</label>";
    html += "<span class='help-block'>Back up your world now, before a big change, say. Your own backups are kept until you delete them.</span>";
    html += "<button type='button' id='" + GENERATE_ARCHIVE_BUTTON_ID + "' class='btn btn-primary'>"
      + "<span class='glyphicon glyphicon-floppy-disk'></span> Back up now</button>";
    html += "</div>";
    html += "<table class='table sortable' id='" + MANUAL_ARCHIVES_TABLE_ID + "'>";
    html += backups_table_head;
    html += "<tbody id='" + MANUAL_ARCHIVES_TBODY_ID + "'></tbody></table></div>";

    html += "<div class='form-group' id='" + CONTENT_ARCHIVES_ERROR_ID + "' style='display:none;'>"
      + "<span class='help-block'>Your list of backups couldn't be loaded. Reload the page to try again.</span></div>";

    // put the base HTML in the content archives panel
    $('#' + Settings.CONTENT_ARCHIVES_PANEL_ID + ' .panel-body').html(html);
  }

  var BACKUP_RESTORE_LINK_CLASS = 'restore-backup';
  var BACKUP_DOWNLOAD_LINK_CLASS = 'download-backup';
  var BACKUP_DELETE_LINK_CLASS = 'delete-backup';
  var ACTIVE_BACKUP_ROW_CLASS = 'active-backup';
  var CORRUPTED_ROW_CLASS = 'danger';

  $('body').on('click',  '.' + BACKUP_DOWNLOAD_LINK_CLASS, function(ev) {
    ev.preventDefault();
    var backupID = $(this).data('backup-id');

    showSpinnerAlert("Getting the backup ready…");
    function checkBackupStatus() {
      $.ajax({
        url: "/api/backups/" + backupID,
        dataType: 'json',
        success: function(data) {
          if (data.complete) {
            if (data.error == '') {
              location.href = "/api/backups/download/" + backupID;
              swal.close();
            } else {
              showErrorMessage(
                "Error",
                "The backup couldn't be got ready to download. Reload the page and try again."
              );
            }
          } else {
            setTimeout(checkBackupStatus, 500);
          }
        },
        error: function() {
          showErrorMessage(
            "Error",
            "The backup couldn't be got ready to download."
          );
        },
      });
    }
    checkBackupStatus();
  });

  function updateProgressBars($progressBar, value) {
    $progressBar.attr('aria-valuenow', value).attr('style', 'width: ' + value + '%');
    $progressBar.find('.ongoing-msg').html(" " + Math.round(value) + "%");
  }

  function reloadBackupInformation() {
    // make a GET request to get backup information to populate the table
    $.ajax({
      url: '/api/backups',
      cache: false
    }).done(function(data) {
      // split the returned data into manual and automatic manual backups
      var splitBackups = _.partition(data.backups, function(value, index) {
        return value.isManualBackup;
      });
      if (data.status.recoveryError && !restoreErrorShown) {
        restoreErrorShown = true;
        swal({
          title: "Error",
          text: "The content couldn't be put back in your world.\n"
          + data.status.recoveryError,
          type: "error",
          showCancelButton: false,
          confirmButtonText: "Restart",
          closeOnConfirm: true,
        },
        function () {
          $.get("/restart");
          showRestartModal();
        });
      }
      if (isRestoring && !data.status.isRecovering && !data.status.recoveryError) {
        // we were recovering and we finished - the DS is going to restart so show the restart modal
        showRestartModal();
        return;
      }

      isRestoring = data.status.isRecovering;

      manualBackups = splitBackups[0];
      automaticBackups = splitBackups[1];

      // populate the backups tables with the backups
      function createBackupTableRow(backup) {
        return "<tr data-backup-id='" + backup.id + "' data-backup-name='" + backup.name + "'>"
          + "<td data-value='" + backup.name.toLowerCase() + "'>" + backup.name + "</td><td data-value='" + backup.createdAtMillis + "'>"
          + moment(backup.createdAtMillis).format('lll')
          + "</td><td class='backup-status'></td><td class='" + ACTION_MENU_CLASS + "'>"
          + "<div class='dropdown'><div class='dropdown-toggle' data-toggle='dropdown' aria-expanded='false'><span class='glyphicon glyphicon-option-vertical'></span></div>"
          + "<ul class='dropdown-menu dropdown-menu-right'>"
          + "<li><a class='" + BACKUP_RESTORE_LINK_CLASS + "' href='#'>Go back to this backup</a></li><li class='divider'></li>"
          + "<li><a class='" + BACKUP_DOWNLOAD_LINK_CLASS + "' data-backup-id='" + backup.id + "' href='#'>Download</a></li><li class='divider'></li>"
          + "<li><a class='" + BACKUP_DELETE_LINK_CLASS + "' href='#' target='_blank'>Delete</a></li></ul></div></td>";
      }

      // before we add any new rows and update existing ones
      // remove our flag for active rows
      $('.' + ACTIVE_BACKUP_ROW_CLASS).removeClass(ACTIVE_BACKUP_ROW_CLASS);

      function updateOrAddTableRow(backup, tableBodyID) {
        // check for a backup with this ID
        var $backupRow = $("tr[data-backup-id='" + backup.id + "']");

        if ($backupRow.length == 0) {
          // create a new row and then add it to the table
          $backupRow = $(createBackupTableRow(backup));
          $('#' + tableBodyID).append($backupRow);
        }

        // update the row status column depending on if it is available or recovering
        if (!backup.isAvailable) {
          // add a progress bar to the status row for availability
          $backupRow.find('td.backup-status').html(progressBarHTML('availability', 'Backing up'));

          // set the value of the progress bar based on availability progress
          updateProgressBars($backupRow.find('.progress-bar'), backup.availabilityProgress * 100);
        } else if (backup.id == data.status.recoveringBackupId) {
          // add a progress bar to the status row for recovery
          $backupRow.find('td.backup-status').html(progressBarHTML('recovery', 'Restoring'));
        } else if (backup.isCorrupted) {
          // add text for corrupted status to row
          $backupRow.find('td.backup-status').html("<span class='dr-backup-damaged' title=\"This backup can't be used: it may be "
            + "empty, or something went wrong while it was made\"><span class='glyphicon glyphicon-warning-sign'></span> Can't be used</span>");
        } else {
          // no special status for this row, use an empty status column
          $backupRow.find('td.backup-status').html('');
        }

        // color the row red if it is corrupted
        $backupRow.toggleClass(CORRUPTED_ROW_CLASS, backup.isCorrupted);

        // disable restore if the backup is corrupted
        $backupRow.find('a.' + BACKUP_RESTORE_LINK_CLASS).parent().toggleClass('disabled', backup.isCorrupted);

        // toggle the dropdown menu depending on if the row is available
        $backupRow.find('td.' + ACTION_MENU_CLASS + ' .dropdown').toggle(backup.isAvailable);

        $backupRow.addClass(ACTIVE_BACKUP_ROW_CLASS);
      }

      if (automaticBackups.length > 0) {
        for (var backupIndex in automaticBackups) {
          updateOrAddTableRow(automaticBackups[backupIndex], AUTOMATIC_ARCHIVES_TBODY_ID);
        }
      }

      if (manualBackups.length > 0) {
        for (var backupIndex in manualBackups) {
          updateOrAddTableRow(manualBackups[backupIndex], MANUAL_ARCHIVES_TBODY_ID);
        }
      }

      // at this point, any rows that no longer have the ACTIVE_BACKUP_ROW_CLASS
      // are deleted backups, so we remove them from the table
      $('#' + CONTENT_ARCHIVES_NORMAL_ID + ' tbody tr:not(.' + ACTIVE_BACKUP_ROW_CLASS + ')').remove();

      // check if the restore action on all rows should be enabled or disabled
      $('tr:not(.' + CORRUPTED_ROW_CLASS + ') .' + BACKUP_RESTORE_LINK_CLASS).parent().toggleClass('disabled', data.status.isRecovering);

      // hide or show the manual content upload file and button depending on our recovering status
      $('#' + UPLOAD_CONTENT_ALLOWED_DIV_ID).toggle(!data.status.isRecovering);
      $('#' + UPLOAD_CONTENT_RECOVERING_DIV_ID).toggle(data.status.isRecovering);

      $('#' + INSTALLED_CONTENT_NAME_ID).text(data.installed_content.name);
      $('#' + INSTALLED_CONTENT_FILENAME_ID).text(data.installed_content.filename);
      $('#' + INSTALLED_CONTENT_CREATED_ID).text(data.installed_content.creation_time ? moment(data.installed_content.creation_time).format('lll') : "");
      $('#' + INSTALLED_CONTENT_INSTALLED_ID).text(data.installed_content.install_time ? moment(data.installed_content.install_time).format('lll') : "");
      $('#' + INSTALLED_CONTENT_INSTALLED_BY_ID).text(data.installed_content.installed_by);
      var installed = !!(data.installed_content.name || data.installed_content.filename);
      $('#installed-content-facts').toggle(installed);
      $('#installed-content-none').toggle(!installed);

      // update the progress bars for current restore status
      if (data.status.isRecovering) {
        updateProgressBars($('.recovery.progress-bar'), data.status.recoveryProgress * 100);
      }

      // tell bootstrap sortable to update for the new rows
      $.bootstrapSortable({ applyLast: true });

      $('#' + CONTENT_ARCHIVES_NORMAL_ID).toggle(true);
      $('#' + CONTENT_ARCHIVES_ERROR_ID).toggle(false);

    }).fail(function(){
      // we've hit the very rare case where we couldn't load the list of backups from the domain server

      // set our backups to empty
      automaticBackups = [];
      manualBackups = [];

      // replace the content archives panel with a simple error message
      // stating that the user should reload the page
      $('#' + CONTENT_ARCHIVES_NORMAL_ID).toggle(false);
      $('#' + CONTENT_ARCHIVES_ERROR_ID).toggle(true);

    }).always(function(){
      // toggle showing or hiding the tables depending on if they have entries
      $('#' + AUTOMATIC_ARCHIVES_TABLE_ID).toggle(automaticBackups.length > 0);
      $('#' + MANUAL_ARCHIVES_TABLE_ID).toggle(manualBackups.length > 0);
    });
  }

  // handle click in table to restore a given content backup
  $('body').on('click', '.' + BACKUP_RESTORE_LINK_CLASS, function(e) {
    // stop the default behaviour
    e.preventDefault();

    // if this is a disabled link, don't proceed with the restore
    if ($(this).parent().hasClass('disabled')) {
      return false;
    }

    // grab the name of this backup so we can show it in alerts
    var backupName = $(this).closest('tr').attr('data-backup-name');

    // grab the ID of this backup in case we need to send a POST
    var backupID = $(this).closest('tr').attr('data-backup-id');

    // make sure the user knows what is about to happen
    swalAreYouSure(
      "Everything in your world now is replaced by the backup " + backupName
        + ". Visitors are disconnected for a moment while the world restarts.",
      "Go back to it",
      function() {
        // show a spinner while we send off our request
        showSpinnerAlert("Going back to " +  backupName);

        // setup an AJAX POST to request content restore
        $.post('/api/backups/recover/' + backupID).done(function(data, textStatus, jqXHR) {
          isRestoring = true;

          // immediately reload our backup information since one should be restoring now
          reloadBackupInformation();

          swal.close();
        }).fail(function(jqXHR, textStatus, errorThrown) {
          showErrorMessage(
            "Error",
            "The backup couldn't be put back in your world.\n"
            + "If this keeps happening, the backup may be damaged."
          );
        });
      }
    )
  });

  // handle click in table to delete a given content backup
  $('body').on('click', '.' + BACKUP_DELETE_LINK_CLASS, function(e){
    // stop the default behaviour
    e.preventDefault();

    // grab the name of this backup so we can show it in alerts
    var backupName = $(this).closest('tr').attr('data-backup-name');

    // grab the ID of this backup in case we need to send the DELETE request
    var backupID = $(this).closest('tr').attr('data-backup-id');

    // make sure the user knows what is about to happen
    swalAreYouSure(
      "The backup " + backupName + " is deleted, and you won't be able to go back to it or download it.",
      "Delete the backup",
      function() {
        // show a spinner while we send off our request
        showSpinnerAlert("Deleting " +  backupName);

        // setup an AJAX DELETE to request content archive delete
        $.ajax({
          url: '/api/backups/' + backupID,
          type: 'DELETE'
        }).done(function(data, textStatus, jqXHR) {
          swal.close();
        }).fail(function(jqXHR, textStatus, errorThrown) {
          showErrorMessage(
            "Error",
            "The backup couldn't be deleted."
          );
        }).always(function(){
          // reload the list of content archives in case we deleted a backup
          // or it's no longer an available backup for some other reason
          reloadBackupInformation();
        });
      }
    )
  });

  // handle click on automatic content archive settings link
  $('body').on('click', '#' + AUTO_ARCHIVES_SETTINGS_LINK_ID, function(e) {
    if (Settings.pendingChanges > 0) {
      // don't follow the link right away, make sure the user knows they are about to leave
      // the page and lose changes
      e.preventDefault();

      var settingsLink = $(this).attr('href');

      swalAreYouSure(
        "You've changed some settings on this page and not saved them. They're lost if you go to the backup schedule now.",
        "Go without saving",
        function() {
          // user wants to drop their changes, switch pages
          window.location =  settingsLink;
        }
      );
    }
  });

  // handle click on manual archive creation button
  $('body').on('click', '#' + GENERATE_ARCHIVE_BUTTON_ID, function(e) {
    e.preventDefault();

    // show a sweet alert to ask the user to provide a name for their content archive
    swal({
      title: "Back up your world",
      type: "input",
      text: "Saves everything in your world as it is right now, so you can go back to it or download it later. Give the backup a name you'll recognise.",
      confirmButtonText: "Back up now",
      showCancelButton: true,
      closeOnConfirm: false,
      inputPlaceholder: 'Before the castle rebuild'
    }, function(inputValue){
      if (inputValue === false) {
        return false;
      }

      if (inputValue === "") {
        swal.showInputError("Give the backup a name.")
        return false;
      }

      var MANUAL_ARCHIVE_NAME_REGEX = /^[a-zA-Z0-9\-_ ]+$/;
      if (!MANUAL_ARCHIVE_NAME_REGEX.test(inputValue)) {
        swal.showInputError("Use letters, numbers, spaces, - and _ only.");
        return false;
      }

      // post the provided archive name to ask the server to kick off a manual backup
      $.ajax({
        type: 'POST',
        url: '/api/backups',
        data: {
          'name': inputValue
        }
      }).done(function(data) {
        // since we successfully setup a new content archive, reload the table of archives
        // which should show that this archive is pending creation
        swal.close();
        reloadBackupInformation();
      }).fail(function(jqXHR, textStatus, errorThrown) {
        showErrorMessage(
          "Error",
          "The backup couldn't be started."
        )
      });
    });
  });

  Settings.extraGroupsAtIndex = Settings.extraContentGroupsAtIndex;

  Settings.afterReloadActions = function() {
    setupBackupUpload();
    setupContentArchives();
    setupInstalledContentInfo();

    // load the latest backups immediately
    reloadBackupInformation();

    // setup a timer to reload them every 5 seconds
    setInterval(reloadBackupInformation, 5000);
  };
});
