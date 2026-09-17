"""Capability inventory separates implementation, configuration and verification."""
import os
from .browser import installed, executable
from .contracts import SERVICES

REAL={
 'web-search':['live_web_search','forecast_weather','historical_weather','public_geocoding','public_road_routing_without_traffic','server_clock','document_files'],
 'health-advice':['persistent_health_records','reminder_create_update_cancel','recurrence_scheduler','notification_inbox','calendar_files'],
 'smart-home':['home_assistant_query_and_control'],
 'device-gui':['app_task_plans','navigation_links_from_supplied_coordinates','android_adb_observe_tap_swipe_type_key_app_launch'],
 'movie-query':['sourced_film_lists','indexed_film_details','web_media_play_pause'],
 'knowledge-base':['public_search','webpage_import','text_import','versioned_corpus_search'],
 'user-memory':['persistent_user_preferences'],
 'multimodal':['provider_image_generation'] if os.environ.get('OPENAI_IMAGE_API_KEY') else [],
 'study-buddy':['model_tutoring','persisted_study_documents'],
 'emotion-chat':['model_conversation','user_requested_self_help_documents'],
 'third-party':['live_web_search'] + (['allowlisted_webhook'] if os.environ.get('BUSINESS_WEBHOOK_URL') or os.environ.get('BUSINESS_WEBHOOK_ALLOWED_ORIGINS') else []),
 'office-work':['docx','xlsx','csv','pdf','pptx','markdown','html','calendar_files','versioned_documents'] + (['smtp_email'] if os.environ.get('BUSINESS_SMTP_HOST') else []),
 'custom-workflow':['save_workflow','execute_tool_steps','per_step_receipts','retry_deduplication'],
 'multi-device':['list_and_select_connected_adb_or_home_assistant_devices'],
 'travel-planner':['live_web_search','persisted_itineraries'],
 'foodie':['live_web_search','persisted_food_plans'],
 'shopping':['live_web_search','persisted_comparisons'],
 'service-manager':['capability_inventory'],
}
MISSING={
 'web-search':['user_geolocation_when_needed','realtime_traffic_data'],
 'health-advice':['real_hospital_account_and_booking_platform','phone_notification_delivery','clinical_diagnosis'],
 'smart-home':['physical_home_or_authorized_home_control_website','original_user_history'],
 'device-gui':['connected_phone_for_native_apps','user_geolocation','authorized_map_account_or_route_api'],
 'movie-query':['platform_login_or_subscription_when_required','physical_tv_or_speaker'],
 'knowledge-base':['private_product_corpus_until_imported','account_entitlement_service'],
 'user-memory':['voiceprint_identity'],
 'multimodal':['video_model_access','speech_transcription_and_synthesis','image_video_understanding'],
 'study-buddy':['school_account_when_needed'],
 'emotion-chat':['professional_or_emergency_service_connection'],
 'third-party':['target_service_login_and_permissions','webhook_configuration_when_needed'],
 'office-work':['online_office_or_email_login_when_needed','smtp_configuration_when_needed'],
 'custom-workflow':['unattended_schedule_and_event_triggers','external_credentials_required_by_steps'],
 'multi-device':['registered_physical_devices'],
 'travel-planner':['reservation_account_when_needed','user_location_when_needed'],
 'foodie':['delivery_address_and_account_when_needed'],
 'shopping':['shopping_account_when_needed','payment_method_when_needed'],
 'service-manager':[],
}

def report():
    from .device_connectors import adb_devices, home_assistant_connected
    android=any(d['status']=='device' for d in adb_devices())
    home=home_assistant_connected()
    ready=installed() and bool(executable())
    table={}
    for name in SERVICES:
        real=REAL[name]
        item={'available':bool(real or ready),'real_actions':real,
              'browser':{'installed':ready,'actions':['observe','click','fill','select','upload','download','media','screenshot'],
                         'execution':'real_chromium','requires_per_site_verification':True,
                         'logged_in': 'user_and_site_specific; never inferred from installation'},
              'missing':MISSING[name]}
        if name=='smart-home' and home:
            item['missing']=[x for x in item['missing'] if not x.startswith('physical_home')]
            item['connected_adapter']='home_assistant'
        if name in ('device-gui','multi-device') and android:
            item['missing']=[x for x in item['missing'] if x not in ('connected_phone_for_native_apps','registered_physical_devices')]
            item['connected_adapter']='android_adb'
        if name=='multimodal' and not os.environ.get('OPENAI_IMAGE_API_KEY'):
            item['missing']=[*item['missing'],'image_credentials']
        if name=='office-work' and os.environ.get('BUSINESS_SMTP_HOST'):
            item['missing']=[x for x in item['missing'] if x!='smtp_configuration_when_needed']
            item['connected_adapter']='smtp'
        if name=='third-party' and (os.environ.get('BUSINESS_WEBHOOK_URL') or os.environ.get('BUSINESS_WEBHOOK_ALLOWED_ORIGINS')):
            item['missing']=[x for x in item['missing'] if x!='webhook_configuration_when_needed']
            item['connected_adapter']='allowlisted_webhook'
        if name in ('smart-home','health-advice','multi-device'):
            item['test_system_actions']=({'smart-home':['stateful_test_home'],
                'health-advice':['test_clinic_inventory_booking'],
                'multi-device':['stateful_virtual_phone_tablet_desktop_tv_speaker_watch','handoff','broadcast','sync']}[name])
            item['test_system_physical_effect']=False
        table[name]=item
    return table

