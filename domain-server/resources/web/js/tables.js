$(document).ready(function(){
  // setup the underscore templates
  var nodeTemplate = _.template($('#nodes-template').html());
  var queuedTemplate = _.template($('#queued-template').html());
  var servicesTemplate = _.template($('#services-template').html());
  var peopleTemplate = _.template($('#people-template').html());

  var SERVICES = Services.list;
  var durationText = Services.durationText;

  function showOverview(nodes, queued) {
    var queuedTypes = _.map(_.values(queued), function(assignment) { return assignment.type; });
    var services = [];
    _.each(SERVICES, function(service) {
      var node = _.find(nodes, function(node) { return node.type === service.type; });
      // a world that doesn't run a service (none connected or waiting) leaves it out
      if (!node && queuedTypes.indexOf(service.type) === -1) {
        return;
      }
      services.push(_.extend({}, service, {
        node: node,
        uptime: node ? durationText(parseFloat(node.uptime)) : ""
      }));
    });
    var running = _.filter(services, function(service) { return service.node; }).length;

    var people = _.map(_.filter(nodes, function(node) { return node.type === "agent"; }), function(node) {
      return _.extend({}, node, { uptimeText: durationText(parseFloat(node.uptime)) });
    });

    $('#dr-service-list').html(servicesTemplate({ services: services }));
    $('#dr-people-list').html(peopleTemplate({ people: people }));

    $('#dr-services-running').text(running + " of " + services.length);
    $('#dr-services-hint').text(services.length === 0 ? "Waiting for the services to start" :
      running === services.length ? "Everything is running" : "Some are still starting");
    $('#dr-people-count').text(people.length);

    var longest = _.max(_.map(_.filter(nodes, function(node) { return node.type !== "agent"; }), function(node) {
      return parseFloat(node.uptime);
    }));
    $('#dr-uptime').text(isFinite(longest) ? durationText(longest) : "–");
  }

  var lastNodes = null;
  var lastQueued = null;
  function updateOverview() {
    if (lastNodes !== null && lastQueued !== null) {
      showOverview(lastNodes, lastQueued);
    }
  }

  // setup a function to grab the assignments
  function getNodesAndAssignments() {
    $.getJSON("nodes.json", function(json){

      json.nodes.sort(function(a, b){
          if (a.type === b.type) {
            if (a.uptime < b.uptime) {
              return 1;
            } else if (a.uptime > b.uptime) {
              return -1;
            } else {
              return 0;
            }
          }

          if (a.type === "agent" && b.type !== "agent") {
            return 1;
          } else if (b.type === "agent" && a.type !== "agent") {
            return -1;
          }

          if (a.type > b.type) {
            return 1;
          }

          if (a.type < b.type) {
            return -1;
          }
      });

      $('#nodes-table tbody').html(nodeTemplate(json));
      lastNodes = json.nodes;
      updateOverview();
    }).fail(function(jqXHR, textStatus, errorThrown) {
      // we assume a 401 means the DS has restarted
      // and no longer has our OAuth produced uuid
      // so just reload and re-auth
      if (jqXHR.status == 401) {
        location.reload();
      }
    });

    $.getJSON("assignments.json", function(json){
      $('#assignments-table tbody').html(queuedTemplate(json));
      lastQueued = json.queued;
      updateOverview();
    }).fail(function(jqXHR, textStatus, errorThrown) {
      // we assume a 401 means the DS has restarted
      // and no longer has our OAuth produced uuid
      // so just reload and re-auth
      if (jqXHR.status == 401) {
        location.reload();
      }
    });
  }

  // a reminder, not a requirement: when nothing locks these pages (no password, and no sign-in with a Darkride
  // account), a banner says so. "Remind me later" hides it in this browser for a week.
  var SIGN_IN_REMINDER_KEY = "darkride.signInReminderHiddenUntil";
  var SIGN_IN_REMINDER_DAYS = 7;

  function signInReminderHidden() {
    try {
      return Number(localStorage.getItem(SIGN_IN_REMINDER_KEY) || 0) > Date.now();
    } catch (e) {
      return false;
    }
  }

  $.getJSON("/settings.json", function(data) {
    var values = data.values || {};
    var security = values.security || {};
    var lockedByAccount = !!(values.oauth && values.oauth.enable);
    if (data.http_password_set === true || lockedByAccount || signInReminderHidden()) {
      return;
    }
    if (security.http_username) {
      $('#dr-sign-in-banner-title').text("These admin pages have a username but no password");
      $('#dr-sign-in-banner-body').text("Anyone who knows the username can change your world's settings, replace its " +
        "content or restart it. Add a password to lock them properly.");
      $('#dr-sign-in-banner .btn-primary').text("Add a password");
    }
    $('#dr-sign-in-banner').show();
  });

  $('#dr-sign-in-banner-later').on('click', function() {
    try {
      localStorage.setItem(SIGN_IN_REMINDER_KEY, String(Date.now() + SIGN_IN_REMINDER_DAYS * 24 * 60 * 60 * 1000));
    } catch (e) {
    }
    $('#dr-sign-in-banner').slideUp(200);
  });

  // do the first GET on page load
  getNodesAndAssignments();
  // grab the new assignments JSON every two seconds
  var getNodesAndAssignmentsInterval = setInterval(getNodesAndAssignments, 2000);

  function deleteNode(uuid) {
    $.ajax({
        url: "/nodes/" + uuid,
        type: 'DELETE',
        success: function(result) {
          console.log("Succesful request to delete node.");
          getNodesAndAssignments();
        }
    });
  }

  // a service that's disconnected starts again by itself
  $(document.body).on('click', '.dr-restart-node', function(){
    var uuid = $(this).data('uuid');
    var name = $(this).data('name');
    swal({
      title: "Restart " + name + "?",
      text: "It stops and starts again by itself. Visitors may notice a short pause.",
      type: "warning",
      showCancelButton: true,
      confirmButtonText: "Restart",
      closeOnConfirm: true
    }, function() {
      deleteNode(uuid);
    });
  });

  // hook the node delete to the X button
  $(document.body).on('click', '.dr-kill-node', function(){
    deleteNode($(this).data('uuid'));
  });

  $('#dr-show-details').on('click', function() {
    var $details = $('#dr-details');
    $details.toggle();
    $(this).text($details.is(':visible') ? "Hide the details" : "Show the details");
  });

  $(document.body).on('click', '#kill-all-btn', function() {
    swal({
      title: "Restart all services?",
      text: "Everyone in your world is disconnected for a moment while the services start again.",
      type: "warning",
      showCancelButton: true,
      confirmButtonText: "Restart all",
      closeOnConfirm: true
    }, function() {
      $.ajax({
        url: "/nodes/",
        type: 'DELETE',
        success: function(result) {
          console.log("Successful request to delete all nodes.");
          getNodesAndAssignments();
        }
      });
    });
  });
});
