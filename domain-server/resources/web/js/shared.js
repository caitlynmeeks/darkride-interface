if (typeof Settings === "undefined") {
  Settings = {};
}

$.extend(Settings, {
  DEPRECATED_CLASS: 'deprecated-setting',
  TRIGGER_CHANGE_CLASS: 'trigger-change',
  DATA_ROW_CLASS: 'value-row',
  DATA_COL_CLASS: 'value-col',
  DATA_CATEGORY_CLASS: 'value-category',
  ADD_ROW_BUTTON_CLASS: 'add-row',
  ADD_ROW_SPAN_CLASSES: 'glyphicon glyphicon-plus add-row',
  DEL_ROW_BUTTON_CLASS: 'del-row',
  DEL_ROW_SPAN_CLASSES: 'glyphicon glyphicon-remove del-row',
  ADD_CATEGORY_BUTTON_CLASS: 'add-category',
  ADD_CATEGORY_SPAN_CLASSES: 'glyphicon glyphicon-plus add-category',
  TOGGLE_CATEGORY_COLUMN_CLASS: 'toggle-category',
  TOGGLE_CATEGORY_SPAN_CLASS: 'toggle-category-icon',
  TOGGLE_CATEGORY_SPAN_CLASSES: 'glyphicon toggle-category-icon',
  TOGGLE_CATEGORY_EXPANDED_CLASS: 'glyphicon-triangle-bottom',
  TOGGLE_CATEGORY_CONTRACTED_CLASS: 'glyphicon-triangle-right',
  DEL_CATEGORY_BUTTON_CLASS: 'del-category',
  DEL_CATEGORY_SPAN_CLASSES: 'glyphicon glyphicon-remove del-category',
  MOVE_UP_BUTTON_CLASS: 'move-up',
  MOVE_UP_SPAN_CLASSES: 'glyphicon glyphicon-chevron-up move-up',
  MOVE_DOWN_BUTTON_CLASS: 'move-down',
  MOVE_DOWN_SPAN_CLASSES: 'glyphicon glyphicon-chevron-down move-down',
  TABLE_BUTTONS_CLASS: 'buttons',
  ADD_DEL_BUTTONS_CLASS: 'add-del-buttons',
  ADD_DEL_BUTTONS_CLASSES: 'buttons add-del-buttons',
  REORDER_BUTTONS_CLASS: 'reorder-buttons',
  REORDER_BUTTONS_CLASSES: 'buttons reorder-buttons',
  NEW_ROW_CLASS: 'new-row',
  CONNECT_ACCOUNT_BTN_ID: 'connect-account-btn',
  DISCONNECT_ACCOUNT_BTN_ID: 'disconnect-account-btn',
  CREATE_DOMAIN_ID_BTN_ID: 'create-domain-btn',
  CHOOSE_DOMAIN_ID_BTN_ID: 'choose-domain-btn',
  GET_TEMPORARY_NAME_BTN_ID: 'get-temp-name-btn',
  DOMAIN_ID_SELECTOR: '[name="metaverse.id"]',
  ACCESS_TOKEN_SELECTOR: '[name="metaverse.access_token"]',
  PLACES_TABLE_ID: 'places-table',
  ADD_PLACE_BTN_ID: 'add-place-btn',
  FORM_ID: 'settings-form',
  INVALID_ROW_CLASS: 'invalid-input',
  DATA_ROW_INDEX: 'data-row-index',
  CONTENT_ARCHIVES_PANEL_ID: 'content_archives',
  UPLOAD_CONTENT_BACKUP_PANEL_ID: 'upload_content',
  INSTALLED_CONTENT: 'installed_content'
});

var URLs = {
  // STABLE METAVERSE_URL: https://vr.darkride.ai
  // STAGING METAVERSE_URL: https://staging.highfidelity.com
  METAVERSE_URL: 'https://vr.darkride.ai',
  // a world's address, opened by Darkride
  PLACE_URL: 'hifi:/',
};

// The light or dark look. The choice (light, dark, or none for Auto: the computer's own setting) is kept in this
// browser; the page's <head> applies it before drawing, and this switches it. Pages that draw in colours of their own
// (the script editor, graphs) hear of a change from a "dr-themechange" event on the document.
var DarkrideTheme = {
  KEY: "darkride.theme",
  MODES: ["auto", "light", "dark"],
  NAMES: { auto: "Auto", light: "Light", dark: "Dark" },

  mode: function() {
    try {
      var mode = localStorage.getItem(DarkrideTheme.KEY);
      return mode === "light" || mode === "dark" ? mode : "auto";
    } catch (e) {
      return "auto";
    }
  },

  // the look in use: light or dark
  current: function() {
    return document.documentElement.getAttribute("data-theme") === "light" ? "light" : "dark";
  },

  apply: function() {
    var mode = DarkrideTheme.mode();
    var light = mode === "light" ||
      (mode === "auto" && !!window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches);
    var theme = light ? "light" : "dark";
    if (theme !== DarkrideTheme.current() || !document.documentElement.hasAttribute("data-theme")) {
      document.documentElement.setAttribute("data-theme", theme);
      $(document).trigger("dr-themechange", [theme]);
    }
  },

  // the next choice: Auto, then Light, then Dark
  cycle: function() {
    var next = DarkrideTheme.MODES[(DarkrideTheme.MODES.indexOf(DarkrideTheme.mode()) + 1) % DarkrideTheme.MODES.length];
    try {
      if (next === "auto") {
        localStorage.removeItem(DarkrideTheme.KEY);
      } else {
        localStorage.setItem(DarkrideTheme.KEY, next);
      }
    } catch (e) {
    }
    DarkrideTheme.apply();
    return next;
  },

  // a colour of the look in use, from the stylesheet's variables (--dr-text, say)
  colour: function(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }
};

// the services a world server runs, in the order the overview shows them, with what each looks after
var Services = {
  list: [
    { type: "entity-server", name: "World objects", about: "Keeps everything in your world and saves it." },
    { type: "avatar-mixer", name: "Avatars", about: "Shares how everyone looks and moves." },
    { type: "audio-mixer", name: "Voice and sound", about: "Mixes voices and sounds so everyone hears what's near them." },
    { type: "asset-server", name: "File storage", about: "Holds the models, images and sounds uploaded to your world." },
    { type: "messages-mixer", name: "Messages", about: "Passes messages between visitors and scripts." },
    { type: "entity-script-server", name: "Server scripts", about: "Runs the scripts attached to objects in your world." },
    { type: "physics-server", name: "Physics", about: "Moves vehicles and other shared physics." }
  ],

  // a service by its type (entity-server), or by the name the stats give it (Entity Server)
  find: function(type) {
    var key = String(type || "").toLowerCase().replace(/ /g, "-");
    for (var i = 0; i < Services.list.length; i++) {
      if (Services.list[i].type === key) {
        return Services.list[i];
      }
    }
    return null;
  },

  // a length of time in words: 45 seconds, 12 minutes, 3 hours, 2 days
  durationText: function(seconds) {
    function plural(n, unit) {
      return n + " " + unit + (n === 1 ? "" : "s");
    }
    seconds = Math.floor(seconds);
    if (seconds < 60) {
      return plural(seconds, "second");
    }
    var minutes = Math.floor(seconds / 60);
    if (minutes < 60) {
      return plural(minutes, "minute");
    }
    var hours = Math.floor(minutes / 60);
    if (hours < 24) {
      return plural(hours, "hour") + (minutes % 60 ? ", " + plural(minutes % 60, "minute") : "");
    }
    var days = Math.floor(hours / 24);
    return plural(days, "day") + (hours % 24 ? ", " + plural(hours % 24, "hour") : "");
  }
};

var Strings = {
  LOADING_SETTINGS_ERROR: "Your settings couldn't be loaded.\nReload the page to try again.",

  CHOOSE_DOMAIN_BUTTON: "Choose from my worlds",
  CREATE_DOMAIN_BUTTON: "Make a new world ID",
  CREATE_DOMAIN_SUCCESS_JUST_CONNECTED: "Your server is connected to Darkride, with a new world ID for this computer.",
  CREATE_DOMAIN_SUCCESS: "This computer has a new world ID.",

  // When a place modification fails, they will be brought back to the previous
  // dialog with new path still set, allowing them to retry immediately, and without
  // having to type the new path in again.
  EDIT_PLACE_TITLE: "Change where visitors arrive",
  EDIT_PLACE_ERROR: "Where visitors arrive couldn't be changed. Try again.",
  EDIT_PLACE_CONFIRM_BUTTON: "Save",
  EDIT_PLACE_CONFIRM_BUTTON_PENDING: "Saving…",
  EDIT_PLACE_CANCEL_BUTTON: "Cancel",

  REMOVE_PLACE_TITLE: "Take <strong>{{place}}</strong> off your world? You keep the name, and can point it here again later.",
  REMOVE_PLACE_ERROR: "The place name couldn't be taken off your world.",
  REMOVE_PLACE_DELETE_BUTTON: "Take it off",
  REMOVE_PLACE_DELETE_BUTTON_PENDING: "Taking it off…",
  REMOVE_PLACE_CANCEL_BUTTON: "Cancel",

  ADD_PLACE_TITLE: "Add a place name",
  ADD_PLACE_MESSAGE: "Choose a place name you own, or register a new one.",
  ADD_PLACE_CONFIRM_BUTTON: "Save",
  ADD_PLACE_CONFIRM_BUTTON_PENDING: "Saving…",
  ADD_PLACE_CANCEL_BUTTON: "Cancel",
  ADD_PLACE_UNKNOWN_ERROR: "The place name couldn't be added. Try saving again.",

  ADD_PLACE_NO_PLACES_MESSAGE: "You don't have any place names yet. Register one on your Darkride account, then come back and add it here.",
  ADD_PLACE_NO_PLACES_BUTTON: "Register a place name",
  ADD_PLACE_UNABLE_TO_LOAD_ERROR: "Your place names couldn't be loaded. Try again in a moment.",
  ADD_PLACE_LOADING_DIALOG: "Fetching your place names…",

  ADD_PLACE_NOT_CONNECTED_TITLE: "Connect to Darkride first",
  ADD_PLACE_NOT_CONNECTED_MESSAGE: "Connect this server to Darkride first: press Connect to Darkride in the Connection section of the settings.",
};

var DOMAIN_ID_TYPE_NONE = 0;
var DOMAIN_ID_TYPE_TEMP = 1;
var DOMAIN_ID_TYPE_FULL = 2;
var DOMAIN_ID_TYPE_UNKNOWN = 3;

function swalAreYouSure(text, confirmButtonText, callback) {
  swal({
    title: "Are you sure?",
    text: text,
    type: "warning",
    cancelButtonText: "Cancel",
    showCancelButton: true,
    confirmButtonText: confirmButtonText,
    closeOnConfirm: false
  }, callback);
}

function domainIDIsSet() {
  if (typeof Settings.data.values.metaverse !== 'undefined' &&
    typeof Settings.data.values.metaverse.id !== 'undefined') {

    return Settings.data.values.metaverse.id.length > 0;
  } else {
    return false;
  }
}

function getCurrentDomainIDType() {
  if (!domainIDIsSet()) {
    return DOMAIN_ID_TYPE_NONE;
  }
  if (typeof DomainInfo === 'undefined') {
    return DOMAIN_ID_TYPE_UNKNOWN;
  }
  if (DomainInfo !== null) {
    if (DomainInfo.name !== undefined) {
      return DOMAIN_ID_TYPE_TEMP;
    }
    return DOMAIN_ID_TYPE_FULL;
  }
  return DOMAIN_ID_TYPE_UNKNOWN;
}

function isCloudDomain() {

  if (!domainIDIsSet()) {
    return false;
  }
  if (typeof DomainInfo === 'undefined') {
    return false;
  }
  if (DomainInfo === null) {
    return false;
  }
  if (typeof DomainInfo.cloud_domain !== "boolean") {
    return false;
  }
  return DomainInfo.cloud_domain;
}

function showLoadingDialog(msg) {
  var message = '<div class="text-center">';
  message += '<span class="glyphicon glyphicon-refresh glyphicon-refresh-animate"></span> ' + msg;
  message += '</div>';

  return bootbox.dialog({
    message: message,
    closeButton: false
  });
}

function sendUpdatePlaceRequest(id, path, domainID, clearDomainID, onSuccess, onError) {
  var data = {
    place_id: id,
    path: path
  };
  if (domainID) {
    data.domain_id = domainID;
  }
  if (clearDomainID) {
    data.domain_id = null;
  }
  $.ajax({
    url: '/api/places',
    type: 'PUT',
    data: data,
    success: onSuccess,
    error: onError
  });
}

var pendingDomainRequest = null;
function getDomainFromAPI(callback) {
  if (pendingDomainRequest !== null) {
    pendingDomainRequest.success(callback);
    pendingDomainRequest.error(function() { callback({ status: 'fail' }) });
    return pendingDomainRequest;
  }

  if (callback === undefined) {
    callback = function() {};
  }

  if (!domainIDIsSet()) {
    callback({ status: 'fail' });
    return null;
  } else {
    var domainID = Settings.data.values.metaverse.id;
  }

  pendingDomainRequest = $.ajax({
    url: "/api/domains/" + domainID,
    dataType: 'json',
    success: function(data) {
      pendingDomainRequest = null;

      if (data.status === 'success') {
        DomainInfo = data.domain;
      } else {
        DomainInfo = null;
      }
      callback(data);
    },
    error: function() {
      pendingDomainRequest = null;

      DomainInfo = null;
      callback({ status: 'fail' });
    }
  });

  return pendingDomainRequest;
}

function chooseFromHighFidelityPlaces(accessToken, forcePathTo, onSuccessfullyAdded) {
  if (accessToken) {

    var loadingDialog = showLoadingDialog(Strings.ADD_PLACE_LOADING_DIALOG);

    function loadPlaces() {
      $.ajax("/api/places", {
        dataType: 'json',
        jsonp: false,
        success: function(data) {
          if (data.status == 'success') {
            var modal_buttons = {
              cancel: {
                label: Strings.ADD_PLACE_CANCEL_BUTTON,
                className: 'add-place-cancel-button btn-default'
              }
            };

            var dialog;
            var modal_body;

            if (data.data.places.length) {
              var places_by_id = {};

              modal_body = $('<div>');

              modal_body.append($("<p>Choose a place name you own, or <a href='" + URLs.METAVERSE_URL + "/user/places' target='_blank'>register a new one</a>.</p>"));

              var currentDomainIDType = getCurrentDomainIDType();
              if (currentDomainIDType === DOMAIN_ID_TYPE_TEMP) {
                var warning = "<div class='domain-loading-error alert alert-warning'>";
                warning += "The name you choose replaces your world's temporary place name.";
                warning += "</div>";
                modal_body.append(warning);
              }

              // setup a select box for the returned places
              modal_body.append($("<label for='place-name-select'>Place name</label>"));
              place_select = $("<select id='place-name-select' class='form-control'></select>");
              _.each(data.data.places, function(place) {
                places_by_id[place.id] = place;
                place_select.append("<option value='" + place.id + "'>" + place.name + "</option>");
              })
              modal_body.append(place_select);
              modal_body.append($("<p id='place-name-warning' class='warning-text' style='display: none'>This name already takes people somewhere. Saving it here points it at your world instead.</p>"));

              if (forcePathTo === undefined || forcePathTo === null) {
                var path = "<div class='form-group'>";
                path += "<label for='place-path-input' class='control-label'>Where they arrive (a path, or / for the usual spot)</label>";
                path += "<input type='text' id='place-path-input' class='form-control' value='/'>";
                path += "</div>";
                modal_body.append($(path));
              }

              var place_select = modal_body.find("#place-name-select")
              place_select.change(function(ev) {
                var warning = modal_body.find("#place-name-warning");
                var place = places_by_id[$(this).val()];
                if (place === undefined || place.pointee === null) {
                  warning.hide();
                } else {
                  warning.show();
                }
              });
              place_select.trigger('change');

              modal_buttons["success"] = {
                label: Strings.ADD_PLACE_CONFIRM_BUTTON,
                className: 'add-place-confirm-button btn btn-primary',
                callback: function() {
                  var placeID = $('#place-name-select').val();
                  // set the place ID on the form
                  $(Settings.place_ID_SELECTOR).val(placeID).change();

                  if (forcePathTo === undefined || forcePathTo === null) {
                    var placePath = $('#place-path-input').val();
                  } else {
                    var placePath = forcePathTo;
                  }

                  $('.add-place-confirm-button').attr('disabled', 'disabled');
                  $('.add-place-confirm-button').html(Strings.ADD_PLACE_CONFIRM_BUTTON_PENDING);
                  $('.add-place-cancel-button').attr('disabled', 'disabled');

                  function finalizeSaveDomainID(domainID) {
                    var jsonSettings = {
                      metaverse: {
                        id: domainID
                      }
                    }
                    var dialog = showLoadingDialog("Waiting for your server to restart…");
                    $.ajax('/settings.json', {
                      data: JSON.stringify(jsonSettings),
                      contentType: 'application/json',
                      type: 'POST'
                    }).done(function(data) {
                      if (data.status == "success") {
                        waitForDomainServerRestart(function() {
                          dialog.modal('hide');
                          if (onSuccessfullyAdded) {
                            onSuccessfullyAdded(places_by_id[placeID].name, domainID);
                          }
                        });
                      } else {
                        bootbox.alert("The place name couldn't be added.");
                      }
                    }).fail(function() {
                      bootbox.alert("The place name couldn't be added.");
                    });
                  }

                  // If domainID is not specified, the current domain id will be used.
                  function finishSettingUpPlace(domainID) {
                    sendUpdatePlaceRequest(
                      placeID,
                      placePath,
                      domainID,
                      false,
                      function(data) {
                        dialog.modal('hide')
                        if (domainID) {
                          $(Settings.DOMAIN_ID_SELECTOR).val(domainID).change();
                          finalizeSaveDomainID(domainID);
                        } else {
                          if (onSuccessfullyAdded) {
                            onSuccessfullyAdded(places_by_id[placeID].name);
                          }
                        }
                      },
                      function(data) {
                        $('.add-place-confirm-button').removeAttr('disabled');
                        $('.add-place-confirm-button').html(Strings.ADD_PLACE_CONFIRM_BUTTON);
                        $('.add-place-cancel-button').removeAttr('disabled');
                        bootbox.alert(Strings.ADD_PLACE_UNKNOWN_ERROR);
                      }
                    );
                  }

                  function maybeCreateNewDomainID() {
                    console.log("Maybe creating domain id", currentDomainIDType)
                    if (currentDomainIDType === DOMAIN_ID_TYPE_FULL) {
                      finishSettingUpPlace();
                    } else {
                      sendCreateDomainRequest(function(domainID) {
                        console.log("Created domain", domainID);
                        finishSettingUpPlace(domainID);
                      }, function() {
                        $('.add-place-confirm-button').removeAttr('disabled');
                        $('.add-place-confirm-button').html(Strings.ADD_PLACE_CONFIRM_BUTTON);
                        $('.add-place-cancel-button').removeAttr('disabled');
                        bootbox.alert(Strings.ADD_PLACE_UNKNOWN_ERROR);
                      });
                    }
                  }

                  maybeCreateNewDomainID();

                  return false;
                }
              }
            } else {
              modal_buttons["success"] = {
                label: Strings.ADD_PLACE_NO_PLACES_BUTTON,
                callback: function() {
                  window.open(URLs.METAVERSE_URL + "/user/places", '_blank');
                }
              }
              modal_body = Strings.ADD_PLACE_NO_PLACES_MESSAGE;
            }

            dialog = bootbox.dialog({
              title: Strings.ADD_PLACE_TITLE,
              message: modal_body,
              closeButton: false,
              buttons: modal_buttons,
              onEscape: true
            });
          } else {
            bootbox.alert(Strings.ADD_PLACE_UNABLE_TO_LOAD_ERROR);
          }
        },
        error: function() {
          bootbox.alert(Strings.ADD_PLACE_UNABLE_TO_LOAD_ERROR);
        },
        complete: function() {
          loadingDialog.modal('hide');
        }
      });
    }

    var domainType = getCurrentDomainIDType();
    if (domainType !== DOMAIN_ID_TYPE_UNKNOWN) {
      loadPlaces();
    } else {
      getDomainFromAPI(function(data) {
        if (data.status === 'success') {
          var domainType = getCurrentDomainIDType();
          loadPlaces();
        } else {
          loadingDialog.modal('hide');
          bootbox.confirm("Your world's details couldn't be fetched from Darkride. Try again?", function(response) {
            if (response) {
              chooseFromHighFidelityPlaces(accessToken, forcePathTo, onSuccessfullyAdded);
            }
          });
        }
      })
    }

  } else {
    bootbox.alert({
      title: Strings.ADD_PLACE_NOT_CONNECTED_TITLE,
      message: Strings.ADD_PLACE_NOT_CONNECTED_MESSAGE
    })
  }
}

function sendCreateDomainRequest(onSuccess, onError) {
  $.ajax({
    url: '/api/domains',
    dataType: 'json',
    type: 'POST',
    data: { label: "" },
    success: function(data) {
      onSuccess(data.domain.id);
    },
    error: onError
  });
}

function waitForDomainServerRestart(callback) {
  function checkForDomainUp() {
    $.ajax('', {
      success: function() {
        callback();
      },
      error: function() {
        setTimeout(checkForDomainUp, 50);
      }
    });
  }

  setTimeout(checkForDomainUp, 10);

}

function prepareAccessTokenPrompt(callback) {
  swal({
    title: "Connect to Darkride",
    type: "input",
    inputPlaceholder: "Paste the access token here",
    text: "Your Darkride worlds opened in a new tab. Make a world there (or pick one), copy its access token, and paste it here." +
      "</br></br>No token yet? Press Cancel, then Connect to Darkride again.</br></br>",
    showCancelButton: true,
    closeOnConfirm: false,
    html: true
  }, function(inputValue){
    if (inputValue === false) {
      return false;
    }

    if (inputValue === "") {
      swal.showInputError("Paste the access token first.")
      return false
    }

    if (callback) {
      callback(inputValue);
    }

    swal.close();
  });
}

function getMetaverseUrl(callback) {
    $.ajax('/api/metaverse_info', {
      success: function(data) {
        callback(data.metaverse_url);
      },
      error: function() {
        callback(URLs.METAVERSE_URL);
      }
    });
}

