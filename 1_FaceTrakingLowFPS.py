import cv2
import time
import numpy as np
import serial
import mediapipe as mp

mp_fase_mesh = mp.solutions.face_mesh
mp_drawing = mp.solutions.drawing_utils
mp_drawing_style = mp.solutions.drawing_styles

URL = "http://172.20.10.2:8080/video"   # poné acá la IP que te muestre IP Webcam
#URL = 1   # webcam USB (dejar comentado si usas el celular)

PUERTO_ARDUINO = 'COM4'                    # verificar en Administrador de dispositivos
BAUDIOS = 115200

wCam, hCam = 640, 480

fotogramas = 20
intervalo_tiempo = 1/fotogramas

# Indices de landmarks de contorno de ojo e iris (MediaPipe Face Mesh, refine_landmarks=True)
LEFT_EYE_IDX = sorted({i for par in mp_fase_mesh.FACEMESH_LEFT_EYE for i in par})
RIGHT_EYE_IDX = sorted({i for par in mp_fase_mesh.FACEMESH_RIGHT_EYE for i in par})
LEFT_IRIS_IDX = [474, 475, 476, 477]
RIGHT_IRIS_IDX = [469, 470, 471, 472]
LEFT_IRIS_CENTER_IDX = 473
RIGHT_IRIS_CENTER_IDX = 468

# Puntos específicos para el cálculo de EAR (6 puntos por ojo)
LEFT_EYE_EAR_IDX = [362, 385, 387, 263, 373, 380]
RIGHT_EYE_EAR_IDX = [33, 160, 158, 133, 153, 144]

UMBRAL_EAR = 0.21
SEGUNDOS_SOMNOLENCIA = 1.0   # tiempo con ojos cerrados para disparar la alerta


def obtener_puntos(face_landmarks, indices, ancho, alto):
	"""Convierte landmarks normalizados (0-1) a coordenadas de pixel."""
	return [(int(face_landmarks.landmark[i].x * ancho), int(face_landmarks.landmark[i].y * alto)) for i in indices]


def obtener_contorno_ojo(face_landmarks, ancho, alto, ojo="izquierdo"):
	"""Devuelve la lista de puntos (x,y) del contorno/parpado del ojo pedido."""
	indices = LEFT_EYE_IDX if ojo == "izquierdo" else RIGHT_EYE_IDX
	return obtener_puntos(face_landmarks, indices, ancho, alto)


def obtener_puntos_iris(face_landmarks, ancho, alto, ojo="izquierdo"):
	"""Devuelve los 4 puntos del borde del iris del ojo pedido."""
	indices = LEFT_IRIS_IDX if ojo == "izquierdo" else RIGHT_IRIS_IDX
	return obtener_puntos(face_landmarks, indices, ancho, alto)


def obtener_centro_iris(face_landmarks, ancho, alto, ojo="izquierdo"):
	"""Devuelve el punto (x,y) del centro del iris del ojo pedido."""
	idx = LEFT_IRIS_CENTER_IDX if ojo == "izquierdo" else RIGHT_IRIS_CENTER_IDX
	lm = face_landmarks.landmark[idx]
	return int(lm.x * ancho), int(lm.y * alto)


def calcular_ear(face_landmarks, indices, ancho, alto):
	"""Calcula el Eye Aspect Ratio (EAR) para un ojo."""
	puntos = obtener_puntos(face_landmarks, indices, ancho, alto)
	p1, p2, p3, p4, p5, p6 = [np.array(p) for p in puntos]

	vertical1 = np.linalg.norm(p2 - p6)
	vertical2 = np.linalg.norm(p3 - p5)
	horizontal = np.linalg.norm(p1 - p4)

	if horizontal == 0:
		return 0.0

	return (vertical1 + vertical2) / (2.0 * horizontal)


def leer_arduino(ser):
	"""Vacia el buffer de entrada e imprime lo que mando el Arduino."""
	if ser is None:
		return
	try:
		while ser.in_waiting:
			linea = ser.readline().decode('utf-8', errors='ignore').strip()
			if linea:
				print("[ARDUINO]", linea)
	except Exception as e:
		print("Error leyendo Arduino:", e)


cap = cv2.VideoCapture(URL)   # camara IP: sin CAP_DSHOW
cap.set(3, wCam)
cap.set(4, hCam)

# Conexion con Arduino
# se abre con DTR desactivado para NO resetear el Nano
# (si se resetea, arranca de nuevo el precalentado de 60s del MQ3)
try:
	arduino = serial.Serial()
	arduino.port = PUERTO_ARDUINO
	arduino.baudrate = BAUDIOS
	arduino.timeout = 1
	arduino.dtr = False
	arduino.open()
	time.sleep(2)
	arduino.reset_input_buffer()
	print(f"Arduino conectado en {PUERTO_ARDUINO}")
except Exception as e:
	arduino = None
	print("No se pudo conectar al Arduino:", e)

Tiempo_Previo = 0
Tiempo_Actual = 0

pTime = time.time()
inicio_ojos_cerrados = None    # marca de tiempo del cierre de ojos
alerta_enviada = False         # para no mandar la señal en chorro

if not cap.isOpened():
	print("ERROR: No se pudo conectar al Dispositivo Movil.")

else:
	face_mesh = mp_fase_mesh.FaceMesh(refine_landmarks=True, max_num_faces=1)

	while True:
		success = cap.grab()

		Tiempo_Actual = time.time()
		if (Tiempo_Actual - Tiempo_Previo) > intervalo_tiempo:
			Tiempo_Previo = Tiempo_Actual

			if not success:
				print("ERROR: Perdida de Conexion.")
				break

			ret, frame = cap.retrieve()
			if not ret or frame is None:      # guarda contra frame vacio
				continue

			# camara del celular: se rota porque filma en vertical
			frame = cv2.cvtColor(cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE), cv2.COLOR_BGR2RGB)
			#frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)   # usar esta si fuera webcam
			results = face_mesh.process(frame)
			frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)

			if results.multi_face_landmarks:
				alto, ancho = frame.shape[:2]
				for face_landmarks in results.multi_face_landmarks:
					mp_drawing.draw_landmarks(
						image=frame,
						landmark_list=face_landmarks,
						connections=mp_fase_mesh.FACEMESH_IRISES,
						landmark_drawing_spec=None,
						connection_drawing_spec=mp_drawing_style.get_default_face_mesh_iris_connections_style()
						)

					contorno_ojo_izq = obtener_contorno_ojo(face_landmarks, ancho, alto, "izquierdo")
					contorno_ojo_der = obtener_contorno_ojo(face_landmarks, ancho, alto, "derecho")
					centro_iris_izq = obtener_centro_iris(face_landmarks, ancho, alto, "izquierdo")
					centro_iris_der = obtener_centro_iris(face_landmarks, ancho, alto, "derecho")

					ear_izq = calcular_ear(face_landmarks, LEFT_EYE_EAR_IDX, ancho, alto)
					ear_der = calcular_ear(face_landmarks, RIGHT_EYE_EAR_IDX, ancho, alto)
					ear_promedio = (ear_izq + ear_der) / 2.0

					# se mide por tiempo real, no por cantidad de frames
					if ear_promedio < UMBRAL_EAR:
						if inicio_ojos_cerrados is None:
							inicio_ojos_cerrados = time.time()

						cerrados = time.time() - inicio_ojos_cerrados
						if cerrados >= SEGUNDOS_SOMNOLENCIA:
							cv2.putText(frame, "SOMNOLENCIA DETECTADA", (30, 130),
								cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 0, 255), 3)
							if arduino is not None and not alerta_enviada:
								arduino.write(b'S')
								print(">>> Señal 'S' enviada al Arduino")
								alerta_enviada = True
					else:
						inicio_ojos_cerrados = None
						alerta_enviada = False   # ojos abiertos: rearmar

					cv2.putText(frame, f'EAR: {ear_promedio:.2f}', (40, 90),
						cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)

			# muestro en consola lo que reporta el Arduino
			leer_arduino(arduino)

			cTime = time.time()
			fps = 1/(cTime - pTime) if cTime > pTime else 0
			pTime = cTime

			cv2.putText(frame, f'FPS: {int(fps)}', (40, 50), cv2.FONT_HERSHEY_COMPLEX, 1, (255, 0, 0), 2)
			cv2.imshow("Captura Original", frame)

		if cv2.waitKey(1) & 0xFF == ord('q'):
			break

	cap.release()
	cv2.destroyAllWindows()
	if arduino is not None:
		arduino.close()