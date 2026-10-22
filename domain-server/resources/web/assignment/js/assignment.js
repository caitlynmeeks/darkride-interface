$(document).ready(function(){

  // setup the editor
  var editor = ace.edit("editor");
  function editorTheme() {
    return DarkrideTheme.current() === "light" ? "ace/theme/darkride-light" : "ace/theme/twilight";
  }
  editor.setTheme(editorTheme());
  $(document).on("dr-themechange", function() {
    editor.setTheme(editorTheme());
  });
  editor.getSession().setMode("ace/mode/javascript");
  editor.getSession().setTabSize(2);

  var messageTimer = null;
  function showMessage(html, isError) {
    clearTimeout(messageTimer);
    $('#run-message').html(html).toggleClass('run-ok', !isError).toggleClass('run-error', !!isError).show();
    messageTimer = setTimeout(function() {
      $('#run-message').fadeOut(300);
    }, isError ? 10000 : 8000);
  }

  $('#deploy-button').click(function(){
    script = editor.getValue();

    // setup our boundary - this is "highfidelity" in hex
    var boundary = "----68696768666964656c697479";
    var body = '--' + boundary + '\r\n'
      // parameter name is "file" and local filename is "temp.txt"
      + 'Content-Disposition:form-data; name="file"; '
      + 'filename="script.js"\r\n'
      // add the javascript mime-type
      + 'Content-type: application/javascript\r\n\r\n'
      // add the script
      + script + '\r\n'
      + '--' + boundary + '--\r\n';

    var headers = {};

    if ($('#instance-field input').val()) {
      headers['ASSIGNMENT-INSTANCES'] = $('#instance-field input').val();
    }

    if ($('#pool-field input').val()) {
      headers['ASSIGNMENT-POOL'] = $('#pool-field input').val();
    }

    var $button = $(this).prop('disabled', true);

    // post form to assignment in order to create an assignment
    $.ajax({
        contentType: "multipart/form-data; boundary=" + boundary,
        data: body,
        type: "POST",
        url: "/assignment",
        headers: headers,
        success: function (data, status) {
          var copies = parseInt(headers['ASSIGNMENT-INSTANCES'] || "1", 10);
          showMessage("<span class='glyphicon glyphicon-ok'></span> Started: " +
            (copies > 1 ? copies + " copies start" : "it starts") + " as soon as a script runner is free. " +
            "See " + (copies > 1 ? "them" : "it") + " under People here now on the <a href='/'>Overview</a>.");
        },
        error: function () {
          showMessage("<span class='glyphicon glyphicon-warning-sign'></span> The script couldn't be sent. " +
            "Check the world server is running, and try again.", true);
        },
        complete: function () {
          $button.prop('disabled', false);
        }
    });
  });
});
