<?php
/**
 * @file
 * Contains batch functions for rendering nodes.
 */

use Apigee\SmartDocs\Method;
use Apigee\SmartDocs\Model;
use Apigee\SmartDocs\Resource;
use Apigee\SmartDocs\Revision;
use Apigee\SmartDocs\Template;

use Apigee\Exceptions\ResponseException;

/**
 * Batch API Function to import nodes.
 *
 * @param Apigee\SmartDocs\Model $model
 *   The model from which nodes will be rendered.
 * @param Apigee\SmartDocs\Revision $revision
 *   The revision from which nodes will be rendered.
 * @param array $selected
 *   An associative array whose keys are Method UUIDs and whose value is either
 *   zero or one. One indicates that the method is to be rendered.
 * @param array $options
 *   An associative array of configuration options, whose key is the option
 *   name and whose value is the option value. At this time the only supported
 *   option is 'publish'.
 * @param bool $render_template
 *   If TRUE, configure the method template before doing anything else.
 *
 * @return array
 *   An array of batch operations to be performed.
 */
function smartdocs_render(Model $model, Revision $revision, $selected, $options, $render_template = TRUE) {
  $display_name = $model->getDisplayName();

  $model_array = $model->toArray();
  $revision_array = $revision->toArray();

  $operations = array();
  if ($render_template) {
    $operations[] = array('smartdocs_template', array($model_array));
  }
  $publish = (bool) $options['publish'];
  if (count($revision->getResources()) > 0) {
    foreach ($selected as $select) {
      if ($select == '0') {
        continue;
      }
      foreach ($revision->getResources() as $resource) {
        $resource_array = $resource->toArray();
        foreach ($resource->getMethods() as $method) {
          if ($select == $method->getUuid()) {
            $operations[] = array(
              'smartdocs_render_nodes',
              array(
                $model_array,
                $revision_array,
                $resource_array,
                $method->toArray(),
                $publish,
              ),
            );
          }
        }
      }
    }
  }

  $batch = array(
    'title' => t('Rendering nodes from the %model model', array('%model' => $display_name)),
    'operations' => $operations,
    'finished' => 'smartdocs_render_finished',
    'init_message' => t('Rendering has started...'),
    'progress_message' => t('Processed @current out of @total.'),
    'error_message' => t('Rendering SmartDocs nodes for %model model has encountered an error.', array('%model' => $display_name)),
    'file' => drupal_get_path('module', 'smartdocs') . '/batch/smartdocs.render.inc',
  );
  return $batch;
}

/**
 * Ensures the model uses the correct template.
 *
 * @param array $model_array
 *   A Model, persisted as an associative array.
 * @param array $context
 *   The current batch context.
 */
function smartdocs_template(array $model_array, array &$context) {
  $model = new Model(devconnect_default_org_config());
  Model::fromArray($model, $model_array);
  if (!in_array($model->getName(), array_keys(node_type_get_names()))) {
    $context['message'] = t('Correct model template verified');

    $path = drupal_get_path('module', 'smartdocs') . '/templates/smartdocs.hbr';
    $custom_template_file = variable_get('smartdocs_custom_template_file', NULL);

    if (!empty($custom_template_file)) {
      $file = file_load($custom_template_file);
      $path = file_create_url($file->uri);
    }
    $html = file_get_contents($path);

    try {
      $template = new Template(devconnect_default_org_config(), $model->getUuid());
      $start = microtime(TRUE);
      $template->save(SMARTDOCS_TEMPLATE_NAME, 'method', $html, TRUE);
      if (variable_get('smartdocs_log_transaction_time', 0)) {
        $args = array(
          '!model' => $model->getUuid(),
          '!time' => number_format(microtime(TRUE) - $start, 4),
        );
        watchdog('smartdocs', 'Saved method template for model !model in !time seconds', $args, WATCHDOG_INFO);
      }
    }
    catch (ResponseException $e) {
      $current_set = &_batch_current_set();
      $current_set['success'] = FALSE;
      $context['results'][] = t('Unable to save the model template. The modeling API says: :response', array(
          ':response' => $e->getCode() . ' ' . $e->getMessage(),
        ));
      $context['finished'] = 1.0;
    }
  }
}

/**
 * Batch operation for creating all of the detected taxonomy terms.
 *
 * @param array $model_array
 *   A Model, persisted as an associative array.
 * @param array $revision_array
 *   A Revision, persisted as an associative array.
 * @param array $resource_array
 *   A Resource, persisted as an associative array.
 * @param array $method_array
 *   A Method, persisted as an associative array.
 * @param bool $publish
 *   If TRUE, the resulting nodes will be published.
 * @param array $context
 *   The current batch context.
 */
function smartdocs_render_nodes(array $model_array, array $revision_array, array $resource_array, array $method_array, $publish, array &$context) {
  $model = new Model(devconnect_default_org_config());
  Model::fromArray($model, $model_array);

  $revision = new Revision($model->getConfig(), $model->getUuid());
  Revision::fromArray($revision, $revision_array);

  $resource = new Resource($model->getConfig(), $model->getUuid(), $revision->getUuid());
  Resource::fromArray($resource, $resource_array);

  $method = new Method($model->getConfig(), $model->getUuid(), $revision->getUuid(), $resource->getUuid());
  Method::fromArray($method, $method_array);
  $method_name = $method->getDisplayName();
  try {
    $start = microtime(TRUE);
    // Reload method in order to pick up tags and other details.
    $method->load();
    if (variable_get('smartdocs_log_transaction_time', 0)) {
      $args = array(
        '!method' => $method_name,
        '!model' => $model->getDisplayName(),
        '!time' => number_format(microtime(TRUE) - $start, 4),
      );
      watchdog('smartdocs', __FUNCTION__ . ': Loaded method !method from model !model in !time seconds.', $args, WATCHDOG_INFO);
    }

  }
  catch (Exception $e) {
    $message = t('Failed to load method :title from the Modeling API.', array(':title' => $method_name));
    $context['message'] = $message;
    $context['results'][] = $message;

    $args = array(
      '!method' =>$method_name,
      '!model' => $model->getName(),
      '!revision' => $revision->getRevisionNumber(),
      '!resource' => $resource->getDisplayName(),
      '!exception' => $e->getMessage(),
    );
    watchdog('smartdocs', "Exception loading method !method from model !model, revision !revision, resource !resource:\n!exception", $args, WATCHDOG_ERROR);
    return;
  }

  $exists = db_select('smartdata', 's')
    ->fields('s', array('nid'))
    ->condition('method', $method->getUuid())
    ->execute()
    ->fetchField();

  if (!$exists) {
    drupal_load('module', 'smartdocs');
    smartdocs_render_node($model, $revision, $resource, $method, $publish, $context);
    $context['message'] = t('Node: :title created', array(':title' => $method_name));
  }
  else {
    $context['message'] = t('Node: :title already created', array(':title' => $method_name));
  }
}

/**
 * Finish function for when SmartDocs Renders.
 *
 * @param bool $success
 *   TRUE if the batch finished successfully, FALSE if errors occurred.
 * @param array $results
 *   Status details for each item in the batch.
 * @param array $operations
 *   The operations that remained unprocessed in case of failure.
 */
function smartdocs_render_finished($success, $results, $operations) {
  if (!$success) {
    drupal_set_message(t('The render failed, consult the logs for more information.'), 'error');
  }
  elseif (!empty($results)) {
    foreach ($results as $message) {
      drupal_set_message($message, 'warning');
    }
  }
}
