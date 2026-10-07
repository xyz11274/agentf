#!/usr/bin/env python3
"""Build native runtime views from the five canonical dataset files."""
from __future__ import annotations
import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
DATASETS = HERE / 'datasets'
WORLD = ROOT / 'examples/agent/business-simulator/appworld/runtime/world'
CACHE = ROOT / 'trace_output/appworld/training/datasets'
FILES = {'train': 'train.jsonl', 'val': 'val.jsonl', 'holdout': 'holdout.jsonl',
         'comparison500': 'comparison-500.jsonl'}
VIEWS = {'native': ('train', 'val'), 'train': ('train',),
         'comparison500': ('comparison500',), 'gepa': ('train', 'val', 'holdout')}


def read(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')


def rows(split: str) -> list[dict]:
    path = DATASETS / FILES[split]
    values = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]
    task_ids = [row['task_id'] for row in values]
    if not values or len(task_ids) != len(set(task_ids)):
        raise RuntimeError(f'dataset_empty_or_duplicate:{split}')
    return values


def public_collection_training_rows() -> list[dict]:
    """Read-only response-container exercises from official public TRAIN only."""
    official = {row['initial_state']['source_task_id']: row for row in rows('train')
                if not row.get('initial_state', {}).get('training_goal')}
    result = []
    for source, collections, threshold in (
            ('07b42fd_1', ['wish_list'], 4.0),
            ('07b42fd_2', ['cart', 'wish_list'], 3.5)):
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        instruction = (
            'Report how many distinct Amazon products and total product units rated at least '
            f'{threshold:g} are currently in my ' + ' and '.join(collections) + '. '
            'Use each product\'s current public rating and count its full quantity in each requested collection. '
            'Submit a JSON object keyed by the requested collection names, with product_count and unit_count '
            'inside each entry, then complete the task. Do not change any cart or wish-list product or quantity. '
            'Inspect the exact public API documentation in a separate code block before reading data. '
            'Check each returned response container against its own documented schema before indexing or iterating; '
            'do not assume that different collection APIs use the same response wrapper.')
        row = deepcopy(official[source])
        row.update(task_id=f'appworld-training-amazon_collection_quantity_summary-{source}',
                   group_id='appworld-training-amazon_collection_quantity_summary',
                   task_input=instruction,
                   expected_behavior='Satisfy this public synthetic TRAIN instruction and its public API state checks; the original source task goal is not evaluated.')
        row['initial_state'].update(training_instruction=instruction,
            training_goal={'kind': 'amazon_collection_quantity_summary',
                           'collections': collections, 'rating_at_least': threshold})
        row['constraints'] += ['Use only public Amazon APIs, preserve the original collections, and compute the requested answer from actual API responses.']
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal')
        result.append(row)
    for original, collection in zip(list(result), ('wish_list', 'cart')):
        row = deepcopy(original)
        method = 'show_wish_list' if collection == 'wish_list' else 'show_cart'
        faulty = ("print(sum(item['quantity'] for item in container['items']))"
                  if collection == 'wish_list' else
                  "print(sum(item['quantity'] for item in container))")
        instruction = (
            'Reproduce and repair this supplied read-only Amazon response-container bug. '
            'Authenticate through the documented public login API and keep its access_token in token. '
            'Execute the supplied faulty snippet exactly once to observe its actual failure; do not repeat it. '
            'Then inspect the exact public response schema in a separate code block and repair the container access. '
            'The faulty snippet is:\n'
            f'container = apis.amazon.{method}(access_token=token)\n{faulty}\n'
            'After the repair, complete this independent public data task: ' + original['task_input'])
        row['task_id'] = row['task_id'].replace('quantity_summary-', 'quantity_summary-debug-')
        row['group_id'] += '-debug'
        row['task_input'] = instruction
        row['initial_state']['training_instruction'] = instruction
        row['initial_state']['training_goal']['reproduce_container_error'] = collection
        result.append(row)
    return result


def public_execution_curriculum_rows() -> list[dict]:
    """Complete public collections and observed cross-app token recovery."""
    official = {row['initial_state']['source_task_id']: row for row in rows('train')
                if not row.get('initial_state', {}).get('training_goal')}
    evaluation = {row['initial_state']['source_task_id']
                  for split in ('val', 'holdout', 'comparison500') for row in rows(split)}
    families = {source.rsplit('_', 1)[0] for source in evaluation}
    specs = (
        ('287e338_2', {'kind': 'spotify_library_unique_artist_histogram', 'min_playlists': 6},
         'Audit the complete Spotify playlist library using only public API responses. '
         'Read the exact playlist and song API schemas in separate code blocks. '
         'Enumerate every playlist page with explicit page_index and page_limit=20, '
         'printing each page index and returned item count until the last page. '
         'Build one global set of song IDs across all playlists before fetching song details. '
         'For every credited artist name, count distinct song IDs from that global set. '
         'Submit a JSON object with playlist_count, distinct_song_count, and '
         'artist_distinct_song_counts (the complete artist-name-to-integer-count mapping). '
         'Do not return only the top artists or only the first page. Preserve all Spotify '
         'playlists and song state, verify the complete counts, and complete the task.'),
        ('b0a8eae_1', {'kind': 'cross_app_auth_recovery_summary', 'require_actual_401': True},
         'Reproduce and repair one real Phone authentication failure, then audit two public '
         'applications without changing their data. First execute exactly once in its own '
         'code block: apis.phone.show_contact_relationships('
         'access_token=str(0)). Observe the actual 401; '
         'do not invent the response or repeat the faulty call. Next inspect the exact '
         'public Phone login and show_contact_relationships documentation in a separate '
         'code block. Obtain the actual supervisor profile and matching application '
         'credentials through supervisor APIs. Phone login uses the profile phone_number '
         'as username; Spotify login uses the profile email as username. Log into Phone '
         'and Spotify separately using each application password, store each actual '
         'login response access_token in a separate variable, and pass only the matching '
         'application token to its protected APIs. Confirm Phone relationship retrieval '
         'succeeds after repair. Enumerate all Phone text messages using query="" and '
         'only_latest_per_contact=False, and all Spotify playlists, with explicit '
         'page_index and page_limit=20 until the final page. Submit a JSON object '
         'with phone_message_count, phone_relationship_count, and spotify_playlist_count. '
         'Verify the full counts, preserve all messages, relationships and playlists, '
         'and complete the task.'),
    )
    result = []
    for source, goal, instruction in specs:
        if source not in official or source in evaluation or source.rsplit('_', 1)[0] in families:
            raise RuntimeError('synthetic_training_public_source_invalid')
        row = deepcopy(official[source]); kind = goal['kind']
        row.update(task_id=f'appworld-training-{kind}-{source}',
                   group_id=f'appworld-training-{kind}', task_input=instruction,
                   expected_behavior='Satisfy only this public TRAIN goal and its public API postconditions; never evaluate the original source task goal.')
        row['initial_state'].update(training_instruction=instruction, training_goal=goal)
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal', execution_curriculum=True)
        result.append(row)
    return result


def public_authentication_training_rows() -> list[dict]:
    """Four one-failure repairs on public TRAIN fixtures and existing goals."""
    official = {row['initial_state']['source_task_id']: row for row in rows('train')
                if not row.get('initial_state', {}).get('training_goal')}
    evaluation_sources = {row['initial_state']['source_task_id']
                          for split in ('val', 'holdout', 'comparison500')
                          for row in rows(split)}
    evaluation_families = {source.rsplit('_', 1)[0] for source in evaluation_sources}
    specs = (
        ('07b42fd_1', 'amazon', 'show_wish_list', 'missing_access_token',
         {'kind': 'amazon_collection_quantity_summary', 'collections': ['wish_list'],
          'rating_at_least': 4.0}),
        ('287e338_1', 'spotify', 'show_playlist_library', 'missing_access_token',
         {'kind': 'spotify_artist_aggregate', 'order': 'most', 'limit': 3,
          'deduplicate_songs': True}),
        ('07b42fd_2', 'amazon', 'show_cart', 'invalid_login_password',
         {'kind': 'amazon_collection_quantity_summary', 'collections': ['cart', 'wish_list'],
          'rating_at_least': 3.5}),
        ('287e338_2', 'spotify', 'show_playlist_library', 'invalid_login_password',
         {'kind': 'spotify_artist_aggregate', 'order': 'least', 'limit': 3,
          'deduplicate_songs': True}),
    )
    result = []
    for source, app, method, failure, goal in specs:
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        if source in evaluation_sources or source.rsplit('_', 1)[0] in evaluation_families:
            raise RuntimeError('synthetic_training_public_source_overlap')
        if failure == 'missing_access_token':
            debug = (
                f'Reproduce and repair one missing access_token argument in a public {app} API call. '
                'Inspect the public login documentation, obtain the current profile and the matching '
                'application credentials through supervisor APIs, and successfully log in first. '
                'Save the actual login response access_token in a variable. '
                'Execute this deliberately faulty protected read exactly once in its own code block: '
                f'print(apis.{app}.{method}()). '
                'Observe its actual failure; do not repeat the faulty call or invent an observation. '
                f'Then read the exact public {method} API documentation in a separate code block, '
                'repair the call by passing access_token from the actual login response, and confirm '
                'that the protected read succeeds. Explicitly pass that token to every later protected API call. ')
        else:
            debug = (
                f'Reproduce and repair one invalid-password login to the public {app} application. '
                'Inspect the exact public login documentation and obtain the current profile\'s '
                'documented login identifier (email or username as specified) through supervisor APIs. '
                'Call login exactly once using that correct documented identifier and the '
                'deliberately incorrect string public_train_intentionally_invalid_password. '
                'Observe the actual 401 authentication failure; do not repeat those credentials '
                'or invent an observation. Then read supervisor show_account_passwords, select the '
                f'credentials belonging to {app}, and use the public profile and login contract to '
                'correct the login. Obtain access_token from the actual successful login response, '
                f'verify a protected {method} read with that token, and explicitly pass it to all '
                'later protected API calls. Never submit credential values as the task answer. ')
        if app == 'amazon':
            task = (
                f'Report how many distinct Amazon products and total product units rated at least '
                f'{goal["rating_at_least"]:g} are currently in my '
                + ' and '.join(goal['collections']) + '. '
                'Use each product\'s current public rating and its full quantity in each requested collection. '
                'Submit a JSON object keyed by the requested collection names, with product_count and '
                'unit_count inside each entry. Preserve every original cart and wish-list product and quantity. ')
        else:
            direction = 'most' if goal['order'] == 'most' else 'least'
            task = (
                f'Across every playlist in my Spotify library, return the {goal["limit"]} artist names '
                f'with the {direction} number of distinct songs. Fetch every playlist page and every '
                'needed song record, count each song ID only once across playlists, sort by count and '
                'normalized artist name, and submit a comma-separated answer. Preserve all Spotify state. ')
        instruction = debug + 'After the repair, complete this independent public data task: ' + task + (
            'Use actual public API responses to compute the answer, verify the requested result, and complete the task.')
        row = deepcopy(official[source])
        row.update(task_id=f'appworld-training-auth-debug-{failure}-{source}',
                   group_id=f'appworld-training-auth-debug-{failure}', task_input=instruction,
                   expected_behavior='Satisfy this public synthetic TRAIN instruction and its existing public API state checks; the original source task goal is not evaluated.')
        row['initial_state'].update(training_instruction=instruction, training_goal=goal)
        row['constraints'] += ['Reproduce the specified authentication failure only once; recover through public API documentation and supervisor APIs.',
                               'Use only public APIs and preserve all application data; never include credentials in the submitted answer.']
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal', training_source_split='train',
            authentication_debug=failure)
        result.append(row)
    return result


def public_observation_training_rows() -> list[dict]:
    """Real no-stdout and repeated-call repairs on public TRAIN fixtures."""
    official = {row['initial_state']['source_task_id']: row for row in rows('train')
                if not row.get('initial_state', {}).get('training_goal')}
    evaluation_sources = {row['initial_state']['source_task_id']
                          for split in ('val', 'holdout', 'comparison500')
                          for row in rows(split)}
    evaluation_families = {source.rsplit('_', 1)[0] for source in evaluation_sources}
    specs = (
        ('07b42fd_1', 'saved_read_result',
         {'kind': 'amazon_collection_quantity_summary', 'collections': ['wish_list'],
          'rating_at_least': 4.0},
         'Practice inspecting a saved public API result without repeating its read. '
         'Inspect the exact public Amazon login and show_wish_list documentation in separate code blocks, '
         'obtain the matching current profile credentials through supervisor APIs, and log in. '
         'Keep the actual login response access_token in token. Execute this read exactly once in its '
         'own code block, with no print or display in that block:\n'
         'observed_collection = apis.amazon.show_wish_list(access_token=token)\n'
         'Observe the actual execution response; do not invent an error merely because there is no stdout. '
         'In the next code block, display the saved observed_collection and its container type, without '
         'calling show_wish_list again. Use its documented response schema. Never display credentials or tokens. '
         'Then report how many distinct products and total product units rated at least 4 are currently '
         'in my Amazon wish_list. Use each product\'s current public rating and its full quantity. '
         'Read additional documented pages only if needed, without repeating an already read page. '
         'Submit a JSON object keyed by wish_list, containing product_count and unit_count, then complete '
         'the task. Preserve every original product, quantity, and all other application state.'),
        ('07b42fd_2', 'silent_mutation_readback', {'kind': 'cart_all_to_wishlist'},
         'Practice confirming a real state change when its code block does not print output. '
         'Move every unit of every product currently in my Amazon cart to my wish list. Preserve all '
         'products and quantities already in the wish list and do not change unrelated data. '
         'Inspect the exact public login, collection-read, and required mutation API documentation in '
         'separate code blocks. Obtain the matching credentials through supervisor APIs and pass the '
         'actual successful login response access_token to every protected call. Gather all original '
         'cart and wish-list IDs and quantities before making changes. Execute one necessary mutation '
         'in its own code block without any print or display, exactly once. Observe its actual response; '
         'no stdout alone does not prove that the state change failed. Do not repeat that mutation merely '
         'to get printed output. Complete the remaining necessary moves using the saved original quantities. '
         'Then perform one directed final verification of the cart and wish list, reading each needed '
         'page once and printing only the non-secret quantities and postconditions. Verify that the cart '
         'is empty and the wish list contains the full original cart quantities plus its preserved '
         'original quantities. If confirmed, stop querying and complete the task. If not confirmed, '
         'report the actual unmet postcondition rather than repeating unchanged calls or claiming success.'),
        ('07b42fd_1', 'repeated_read_saved_result',
         {'kind': 'amazon_collection_quantity_summary', 'collections': ['wish_list'],
          'rating_at_least': 4.0},
         'Reproduce and repair a missing-display mistake in a public Amazon read. '
         'Inspect the exact public login and show_wish_list API documentation in separate code blocks, '
         'obtain the matching credentials through supervisor APIs, log in, and keep its actual access_token '
         'in token. In one code block save the result without printing it:\n'
         'observed_collection = apis.amazon.show_wish_list(access_token=token)\n'
         'Observe the actual execution response. To reproduce the supplied mistake, in the next code '
         'block repeat that same read exactly once, also without printing, storing it in repeated_collection. '
         'Do not repeat it again or invent an execution error. In a separate next block display the '
         'already saved observed_collection and its container type without calling the API again. '
         'Use the actual displayed response and public schema to distinguish execution success from '
         'whether the result was shown. Never display credentials or tokens. Then report how many '
         'distinct products and total product units rated at least 4 are currently in my Amazon wish_list, '
         'using each current public rating and full quantity. Fetch only genuinely unread needed pages. '
         'Submit a JSON object keyed by wish_list with product_count and unit_count and complete the task. '
         'Preserve all original application data; do not keep querying once the requested answer is verified.'),
        ('07b42fd_2', 'repeated_mutation_readback', {'kind': 'cart_all_to_wishlist'},
         'Reproduce and recover from one unnecessary repeated Amazon cart mutation. Move every unit '
         'of every product originally in my cart to my wish list, preserving every original wish-list '
         'product and quantity and all unrelated state. Inspect the exact public login, collection-read, '
         'and move_product_from_cart_to_wish_list documentation in separate code blocks. Obtain matching '
         'credentials through supervisor APIs and pass the actual successful access_token to every '
         'protected call. Read and save all original cart and wish-list IDs and quantities. Select one '
         'actual cart product and call move_product_from_cart_to_wish_list with its full original quantity '
         'in a separate block without print. After its actual successful response, reproduce the supplied '
         'mistake by calling that identical mutation with the same product_id, quantity, and token exactly '
         'once more in another block. Observe and report its actual API response or rejection; do not '
         'invent an error if it returns successfully, and do not repeat the mutation again. Use one '
         'directed public readback to determine the actual resulting state before deciding whether any '
         'repair is necessary. Complete only the remaining required moves from the saved original '
         'quantities. Perform one final directed verification of cart and wish list, print non-secret '
         'postconditions, and complete the task as soon as the cart is empty and the wish list has its '
         'preserved original quantities plus the full original cart quantities. If a postcondition is '
         'unmet, use its actual evidence to repair it; do not repeat unchanged writes merely to get stdout.'),
    )
    result = []
    for source, exercise, goal, instruction in specs:
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        if source in evaluation_sources or source.rsplit('_', 1)[0] in evaluation_families:
            raise RuntimeError('synthetic_training_public_source_overlap')
        row = deepcopy(official[source])
        row.update(task_id=f'appworld-training-observation-debug-{exercise}-{source}',
                   group_id=f'appworld-training-observation-debug-{exercise}', task_input=instruction,
                   expected_behavior='Satisfy this public synthetic TRAIN instruction and its existing public API state checks; the original source task goal is not evaluated.')
        row['initial_state'].update(training_instruction=instruction, training_goal=goal)
        debug_constraint = (
            'Reproduce only the single requested duplicate call; inspect actual saved results and state, never invent an error, and do not repeat it further.'
            if exercise.startswith('repeated_') else
            'Execute the specified no-stdout block once; inspect its actual response and saved state without inventing an error or repeating unchanged calls.')
        row['constraints'] += [debug_constraint,
                               'Use only public APIs and never display or submit credentials or tokens.']
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal', training_source_split='train',
            observation_debug=exercise)
        result.append(row)
    return result


def ensure_training_augmentation() -> list[str]:
    """Append public-training exercises; never alter official task bodies."""
    path = DATASETS / FILES['train']
    original = path.read_text(encoding='utf-8')
    values = rows('train')
    official = {row['initial_state']['source_task_id']: row for row in values
                if not row.get('initial_state', {}).get('training_goal')}
    existing_rows = {row['task_id']: row for row in values}
    existing = set(existing_rows)
    generated = []
    for source in ('07b42fd_1', '07b42fd_2', '07b42fd_3'):
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        for kind, instruction in (
            ('cart_all_to_wishlist',
             'Move every unit of every product currently in my Amazon cart to my wish list. '
             'Preserve all products and quantities already in the wish list and do not change unrelated data. '
             'Verify the cart is empty and that the wish list contains the full original quantities before completing the task.'),
            ('archive_read_nonstarred_inbox',
             'Archive every currently unarchived, non-spam Gmail inbox thread that is read and not starred. '
             'Include all result pages, preserve every other thread and its properties, and verify the requested final state before completing the task.')):
            instruction += (' Review the exact public API documentation in a separate code block before making changes. '
                            'For paginated collections, collect all target IDs before changing any of them.')
            native_id = f'appworld-training-{kind}-{source}'
            generated.append(native_id)
            if native_id in existing:
                continue
            row = deepcopy(official[source])
            row.update(task_id=native_id,
                       group_id=f'appworld-training-{kind}',
                       task_input=instruction,
                       expected_behavior='Satisfy this synthetic training instruction and its public API state checks; the original source task goal is not evaluated.')
            row['constraints'] += ['Discover exact public API schemas before executing state-changing operations; inspect documentation in a separate code block.',
                                   'For paginated collections, gather the complete target set before mutating it.']
            row['initial_state'].update(training_instruction=instruction, training_goal={'kind': kind})
            row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
            row['attributes']['appworld'].update(synthetic_training=True,
                reward_source='public_synthetic_training_goal')
            values.append(row)
    source = '07b42fd_1'
    native_id = f'appworld-training-order_selected_wishlist_preserve_cart-{source}'
    generated.append(native_id)
    if native_id not in existing:
        instruction = (
            'Place one Amazon order for every unit of every soap pump currently in my wish list. '
            'Keep every original cart product and its full quantity in the cart after ordering, '
            'and preserve all non-target wish-list products and quantities. Do not order any non-target product. '
            'Inspect the exact public API documentation in a separate code block first. '
            'Verify the new order contains exactly the selected wish-list products and their full original quantities, '
            'and verify that the original cart and remaining wish list are preserved before completing the task.')
        row = deepcopy(official[source])
        row.update(task_id=native_id, group_id='appworld-training-order_selected_wishlist_preserve_cart',
                   task_input=instruction,
                   expected_behavior='Satisfy this synthetic training instruction and its public API state checks; the original source task goal is not evaluated.')
        row['constraints'] += ['Use only the public Amazon API; verify the entire order and preserved cart/wishlist quantities.']
        row['initial_state'].update(training_instruction=instruction,
            training_goal={'kind': 'order_selected_wishlist_preserve_cart', 'product_name_contains': 'soap pump'})
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal')
        values.append(row)
    for source, threshold in (('07b42fd_1', 4.8), ('07b42fd_3', 3.5), ('07b42fd_2', 4.2)):
        kind = 'cart_selected_rating_to_wishlist'
        native_id = f'appworld-training-{kind}-{source}'
        generated.append(native_id)
        if native_id not in existing:
            instruction = (
                f'Move every unit of each Amazon cart product rated below {threshold:g} to my wish list. '
                'Keep all other cart products and their original quantities unchanged, and preserve every '
                'product and quantity already in the wish list. Read the exact public API documentation '
                'before changing state. Inspect the initial cart and product ratings, collect the target '
                'set before moving anything, and verify that no target units remain in the cart and that '
                'the wish list gained their full original quantities before completing the task.')
            row = deepcopy(official[source])
            row.update(task_id=native_id, group_id=f'appworld-training-{kind}', task_input=instruction,
                       expected_behavior='Satisfy this synthetic training instruction and its public API state checks; the original source task goal is not evaluated.')
            row['constraints'] += ['Move only the requested subset, preserve full quantities, and verify target residuals before completing.']
            row['initial_state'].update(training_instruction=instruction,
                training_goal={'kind': kind, 'rating_below': threshold})
            row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
            row['attributes']['appworld'].update(synthetic_training=True,
                reward_source='public_synthetic_training_goal')
            values.append(row)
    # Cross-application playlist editing is deliberately a separate public
    # training contract.  The source tasks are official TRAIN rows; only the
    # instruction and public synthetic goal are changed, so no val/holdout
    # task is reused or modified.
    playlist_edit_instruction = (
        'Read the complete suggestion conversation in my Phone messages, find the linked Spotify playlist, '
        'and apply every explicit add and remove suggestion exactly once. Resolve each song name to its public '
        'Spotify song ID before changing the playlist. Preserve every unrelated song and playlist, verify the '
        'final playlist equals the original set plus all requested additions minus all requested removals, '
        'and then complete the task. Inspect the exact public Phone and Spotify API schemas first; paginate '
        'message and playlist reads before any mutation.')
    for source in ('b0a8eae_1', 'b0a8eae_2', 'b0a8eae_3'):
        kind = 'cross_app_playlist_edit'
        native_id = f'appworld-training-{kind}-{source}'
        generated.append(native_id)
        if native_id in existing:
            continue
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        row = deepcopy(official[source])
        row.update(task_id=native_id, group_id=f'appworld-training-{kind}',
                   task_input=playlist_edit_instruction,
                   expected_behavior='Satisfy this synthetic training instruction and its public API state checks; the original source task goal is not evaluated.')
        row['constraints'] += [
            'Use only public Phone and Spotify APIs; collect the complete message and playlist evidence before mutation.',
            'Apply the exact add/remove set and preserve unrelated playlist state.']
        row['initial_state'].update(
            training_instruction=playlist_edit_instruction,
            training_goal={'kind': kind, 'require_all_recommendations': True})
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal')
        values.append(row)

    # Artist aggregation is another independent public contract.  It teaches
    # complete playlist pagination, cross-playlist song de-duplication and a
    # deterministic top/least-k answer without exposing any evaluation data.
    artist_aggregate_specs = (
        ('287e338_1', 'most', 3),
        ('287e338_2', 'least', 3),
        ('287e338_3', 'most', 5),
    )
    for source, order, limit in artist_aggregate_specs:
        kind = 'spotify_artist_aggregate'
        native_id = f'appworld-training-{kind}-{source}'
        generated.append(native_id)
        if native_id in existing:
            continue
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        direction = 'most' if order == 'most' else 'least'
        instruction = (
            f'Across every playlist in my Spotify library, return the {limit} artist names with the {direction} '
            'number of distinct songs. Fetch every playlist page and every song record needed to resolve artist '
            'names, count each song ID only once even when it appears in multiple playlists, sort deterministically '
            'by count and normalized artist name, submit a comma-separated answer, and complete the task. '
            'Do not change any Spotify state. Inspect the exact public Spotify API schemas before reading data.')
        row = deepcopy(official[source])
        row.update(task_id=native_id, group_id=f'appworld-training-{kind}',
                   task_input=instruction,
                   expected_behavior='Satisfy this synthetic training instruction and its public API state checks; the original source task goal is not evaluated.')
        row['constraints'] += [
            'Use only public Spotify APIs; paginate the complete playlist library before aggregation.',
            'De-duplicate song IDs across playlists and preserve all state because this is a read-only task.']
        row['initial_state'].update(
            training_instruction=instruction,
            training_goal={'kind': kind, 'order': order, 'limit': limit,
                           'deduplicate_songs': True})
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal')
        values.append(row)
    for row in (public_collection_training_rows() + public_authentication_training_rows()
                + public_observation_training_rows() + public_execution_curriculum_rows()):
        generated.append(row['task_id'])
        if (row['attributes']['appworld'].get('authentication_debug')
                and row['task_id'] in existing
                and existing_rows[row['task_id']] != row):
            raise RuntimeError('synthetic_training_auth_task_conflict')
        if (row['attributes']['appworld'].get('observation_debug')
                and row['task_id'] in existing
                and existing_rows[row['task_id']] != row):
            raise RuntimeError('synthetic_training_observation_task_conflict')
        if row['task_id'] not in existing:
            values.append(row)
    additions = values[len(rows('train')):]
    if additions:
        suffix = ''.join(json.dumps(row, ensure_ascii=False, separators=(',', ':')) + '\n' for row in additions)
        temporary = path.with_name(path.name + '.new')
        temporary.write_text(original + ('' if original.endswith('\n') else '\n') + suffix, encoding='utf-8')
        temporary.replace(path)
        # Keep the single canonical manifest truthful after public training
        # augmentation.  The manifest is a compact count receipt, not a
        # second dataset version or fingerprint.
        manifest_path = DATASETS / 'manifest.json'
        manifest = read(manifest_path)
        manifest['counts'] = {split: len(rows(split)) for split in FILES}
        write(manifest_path, manifest)
    return generated


def freeze() -> dict:
    """Compatibility view in memory; no extra selection file is generated."""
    manifest = read(DATASETS / 'manifest.json')
    return dict(manifest, dataset='appworld-agentflow',
                train_ids=[row['task_id'].removeprefix('appworld-') for row in rows('train')],
                dev_ids=[row['task_id'].removeprefix('appworld-') for row in rows('val')],
                final_ids=[row['task_id'].removeprefix('appworld-') for row in rows('comparison500')])


def pending_training_rows() -> list[dict]:
    """Nine independent public TRAIN exercises; leave the active pack intact."""
    official = {row['initial_state']['source_task_id']: row for row in rows('train')
                if not row.get('initial_state', {}).get('training_goal')}
    specs = []
    # Thresholds are feasible in these public TRAIN fixtures (maximum partial
    # downloaded durations are 510, 211 and 279 seconds respectively).
    for source, seconds in (('229360a_1', 450), ('229360a_2', 180), ('229360a_3', 240)):
        specs.append((source, {'kind': 'offline_album_downloaded_duration',
                              'min_duration_seconds': seconds},
            f'Play an existing album from my Spotify album library for an offline trip lasting {seconds} seconds. '
            'The songs already downloaded from that album must together last at least this long without repetition. '
            'Choose an album that also contains some songs that are not downloaded. Do not download anything or change any library. '
            'Read all album-library and downloaded-song pages, calculate the downloaded subset duration for each album, '
            'start one qualifying album, verify playback and preserved downloads, then complete the task.'))
    for source, genres in (('b7a9ee9_1', ['classical', 'reggae']),
                           ('b7a9ee9_2', ['reggae', 'indie']),
                           ('b7a9ee9_3', ['indie', 'classical'])):
        specs.append((source, {'kind': 'spotify_genre_artist_set', 'genres': genres},
            'Follow every artist credited on any song in any of my Spotify library playlists whose genre is '
            + ' or '.join(genres) + '. Treat these genres as a set of alternatives and compare normalized exact genre names. '
            'Read every playlist page and needed song record, deduplicate song and artist IDs, preserve all previous '
            'artist follows, do not follow artists from other songs, and preserve every playlist. Verify the final '
            'artist-follow set before completing the task.'))
    for source, collection, direction, days in (
            ('afc0fce_1', 'transactions', 'received', 5),
            ('afc0fce_2', 'transactions', 'sent', 7),
            ('afc0fce_3', 'payment_requests', 'received', 10)):
        label = ('payments I actually ' + direction if collection == 'transactions'
                 else 'payment requests I received from other people, regardless of request status')
        specs.append((source, {'kind': 'venmo_date_direction_total', 'collection': collection,
                              'direction': direction, 'days': days},
            f'What is the total dollar amount of all Venmo {label} in the last {days} calendar days including today? '
            f'Use the public task datetime: the first included date is today minus {days - 1} days and the final '
            'included date is today, with both entire dates included. Paginate the complete relevant collection. '
            'Distinguish payment requests from completed transactions and use the requested direction. '
            'Do not modify any Venmo data. Submit only the numeric dollar total as the task answer, then complete the task.'))
    result = []
    for source, goal, instruction in specs:
        if source not in official:
            raise RuntimeError('synthetic_training_public_source_missing')
        row = deepcopy(official[source])
        kind = goal['kind']
        instruction += ' Inspect the exact public API documentation in a separate code block before using the APIs.'
        row.update(task_id=f'appworld-training-pending-{kind}-{source}',
                   group_id=f'appworld-training-pending-{kind}', task_input=instruction,
                   expected_behavior='Satisfy this independent public synthetic TRAIN goal; the original source task goal is not evaluated.')
        row['initial_state'].update(training_instruction=instruction, training_goal=goal)
        row['constraints'] += ['Use only public APIs; gather complete target collections and verify the requested final state.']
        row['provenance'] = {'kind': 'synthetic', 'learning_exposed': True}
        row['attributes']['appworld'].update(synthetic_training=True,
            reward_source='public_synthetic_training_goal')
        result.append(row)
    return result


def prepare_pending_training(binary: Path, destination: Path) -> dict:
    """Materialize an isolated reviewed TRAIN pack without replacing any view."""
    destination = destination.resolve()
    tasks = pending_training_rows()
    source_text = ''.join(json.dumps(row, ensure_ascii=False, separators=(',', ':')) + '\n'
                          for row in tasks)
    if destination.exists():
        if ((destination / 'public-train.jsonl').read_text() != source_text
                or not (destination / 'native/manifest.json').is_file()):
            raise RuntimeError('pending_training_destination_conflict')
        return {'path': str(destination / 'native'), 'public_train': str(destination / 'public-train.jsonl'),
                'task_ids': [row['task_id'] for row in tasks], 'reused': True}
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.pending-train-', dir=destination.parent) as directory:
        staging = Path(directory)
        (staging / 'public-train.jsonl').write_text(source_text, encoding='utf-8')
        write(staging / 'dataset.json', {
            'schema_version': 1, 'name': 'appworld-public-pending-train',
            'split_seed': 'appworld-canonical', 'split_percent': [1, 1, 98],
            'max_tasks': len(tasks), 'max_model_calls': 0,
            'budget': {'max_steps': 300, 'max_tool_calls': 300, 'timeout_ms': 1200000},
            'sources': [{'kind': 'golden', 'path': 'public-train.jsonl'}]})
        built = staging / 'native'
        subprocess.run([str(binary), 'dataset', 'build', '--config', str(staging / 'dataset.json'),
                        '--output-root', str(built)], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        subprocess.run([str(binary), 'dataset', 'check', str(built), '--require-reviewed'],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        write(staging / 'pending.json', {'status': 'pending_real_agent_capture',
              'scope': 'public_synthetic_train', 'task_ids': [row['task_id'] for row in tasks],
              'source_task_ids': [row['initial_state']['source_task_id'] for row in tasks]})
        staging.rename(destination)
    return {'path': str(destination / 'native'), 'public_train': str(destination / 'public-train.jsonl'),
            'task_ids': [row['task_id'] for row in tasks], 'reused': False}


def activate_pending_training(destination: Path) -> list[str]:
    """Append a prepared public TRAIN pack for the normal incremental runner."""
    pending = pending_training_rows()
    source = destination / 'public-train.jsonl'
    prepared = [json.loads(line) for line in source.read_text().splitlines() if line.strip()]
    if prepared != pending or not (destination / 'native/manifest.json').is_file():
        raise RuntimeError('pending_training_pack_not_prepared')
    active = rows('train')
    existing = {row['task_id']: row for row in active}
    if any(row['task_id'] in existing and existing[row['task_id']] != row for row in pending):
        raise RuntimeError('pending_training_active_task_conflict')
    additions = [row for row in pending if row['task_id'] not in existing]
    if additions:
        path = DATASETS / FILES['train']
        original = path.read_text(encoding='utf-8')
        suffix = ''.join(json.dumps(row, ensure_ascii=False, separators=(',', ':')) + '\n'
                         for row in additions)
        temporary = path.with_name(path.name + '.new')
        temporary.write_text(original + ('' if original.endswith('\n') else '\n') + suffix,
                             encoding='utf-8')
        temporary.replace(path)
        manifest = read(DATASETS / 'manifest.json')
        manifest['counts'] = {split: len(rows(split)) for split in FILES}
        write(DATASETS / 'manifest.json', manifest)
    return [row['task_id'] for row in additions]


def task_ref(task_id: str, split: str) -> dict:
    native_id = 'appworld-' + task_id.removeprefix('appworld-')
    for source in ('train', 'val', 'holdout'):
        for row in rows(source):
            if row['task_id'] == native_id:
                value = deepcopy(row)
                value['split'] = 'val' if split == 'dev' else split
                return value
    raise RuntimeError(f'canonical_task_missing:{native_id}')


def validate() -> dict:
    values = {split: rows(split) for split in FILES}
    ids = {split: {row['task_id'] for row in items} for split, items in values.items()}
    for left, right in (('train', 'val'), ('train', 'holdout'), ('val', 'holdout')):
        if ids[left] & ids[right]:
            raise RuntimeError(f'dataset_split_overlap:{left}:{right}')
    if ids['holdout'] != ids['comparison500'] or len(ids['comparison500']) != 500:
        raise RuntimeError('comparison500_scope_invalid')
    manifest = read(DATASETS / 'manifest.json')
    if manifest.get('selection_uses_scores') is not False:
        raise RuntimeError('dataset_selection_policy_missing')
    declared = manifest.get('counts') or {}
    actual = {split: len(items) for split, items in values.items()}
    if any(declared.get(split) != count for split, count in actual.items()):
        raise RuntimeError('dataset_manifest_count_mismatch')
    return actual


def materialize(binary: Path, *, view: str = 'native',
                frozen_training_tasks: list[str] | None = None,
                expected_dataset_version: str | None = None) -> dict:
    counts = validate()
    frozen = frozen_training_tasks is not None or expected_dataset_version is not None
    if frozen:
        training = rows('train')
        if (view != 'gepa' or not isinstance(frozen_training_tasks, list)
                or not frozen_training_tasks or any(not isinstance(task, str) or not task for task in frozen_training_tasks)
                or len(set(frozen_training_tasks)) != len(frozen_training_tasks)
                or not isinstance(expected_dataset_version, str) or not expected_dataset_version
                or frozen_training_tasks != [row['task_id'] for row in training[:len(frozen_training_tasks)]]):
            raise RuntimeError('appworld_frozen_gepa_training_scope_invalid')
    def selected(source):
        values = rows(source)
        return values[:len(frozen_training_tasks)] if frozen and source == 'train' else values
    destination = CACHE / view
    # Reuse healthy captures, but materialize actual newly added task IDs.
    # This is a scope check, not a version/fingerprint gate.
    expected_ids = {row['task_id'] for source in VIEWS[view] for row in selected(source)}
    if (destination / 'manifest.json').is_file():
        cached_ids = {json.loads(line)['task_id'] for split in ('train', 'val', 'holdout')
                      for line in (destination / (split + '.jsonl')).read_text().splitlines() if line.strip()}
        cached_version = read(destination / 'manifest.json').get('metadata', {}).get('dataset_version')
        if cached_ids == expected_ids and (not frozen or cached_version == expected_dataset_version):
            return {'path': str(destination), 'reused': True, 'counts': counts}
    CACHE.mkdir(parents=True, exist_ok=True)
    tasks = []
    for source in VIEWS[view]:
        for item in selected(source):
            item = deepcopy(item)
            item['split'] = 'holdout' if source == 'comparison500' else source
            tasks.append(item)
    with tempfile.TemporaryDirectory(prefix='.build-', dir=CACHE) as directory:
        staging = Path(directory)
        (staging / 'tasks.jsonl').write_text(''.join(json.dumps(row, ensure_ascii=False, separators=(',', ':')) + '\n' for row in tasks), encoding='utf-8')
        write(staging / 'dataset.json', {
            'schema_version': 1, 'name': 'appworld-' + view,
            'split_seed': 'appworld-canonical', 'split_percent': [1, 1, 98],
            'max_tasks': len(tasks), 'max_model_calls': 0,
            'budget': {'max_steps': 300, 'max_tool_calls': 300, 'timeout_ms': 1200000},
            'sources': [{'kind': 'golden', 'path': 'tasks.jsonl'}]})
        built = staging / 'built'
        result = subprocess.run([str(binary), 'dataset', 'build', '--config', str(staging / 'dataset.json'), '--output-root', str(built)], cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError((result.stderr or result.stdout)[-3000:])
        # Verify required fields/splits once at creation, not a resume version gate.
        subprocess.run([str(binary), 'dataset', 'check', str(built), '--require-reviewed'], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        if frozen and read(built / 'manifest.json').get('metadata', {}).get('dataset_version') != expected_dataset_version:
            raise RuntimeError('appworld_frozen_gepa_dataset_version_mismatch')
        if destination.exists():
            old_view = staging / 'previous-view'
            destination.rename(old_view)
            try:
                built.rename(destination)
            except BaseException:
                old_view.rename(destination)
                raise
        else:
            built.rename(destination)
    return {'path': str(destination), 'reused': False, 'counts': counts}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--view', choices=tuple(VIEWS), default='native')
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--pending-training-root', type=Path)
    parser.add_argument('--activate-pending-training-root', type=Path)
    args = parser.parse_args()
    if args.check:
        print(json.dumps(validate())); return 0
    if args.activate_pending_training_root is not None:
        print(json.dumps({'added_task_ids': activate_pending_training(args.activate_pending_training_root),
                          'counts': validate()})); return 0
    import layout
    if args.pending_training_root is not None:
        print(json.dumps(prepare_pending_training(layout.BINARY, args.pending_training_root))); return 0
    views = tuple(VIEWS) if args.all else (args.view,)
    print(json.dumps({view: materialize(layout.BINARY, view=view) for view in views}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
